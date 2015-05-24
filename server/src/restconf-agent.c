/**
 * @file agent.c
 * @author David Kupka <xkupka01@stud.fit.vutbr.cz>
 * @brief NETCONF agent. Starts as ssh subsystem, performs handshake and passes
 * messages between server and client.
 *
 * Copyright (c) 2011, CESNET, z.s.p.o.
 * All rights reserved.
 *
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the CESNET, z.s.p.o. nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <alloca.h>
#include <ctype.h>

#include <libxml/tree.h>

#include <libnetconf_xml.h>
#include <libnetconf.h>

#include "common.c"
#include "comm.h"
#include "http_parser.h"
#include "converter.h"

/* Define libnetconf submodules necessary for the NETCONF agent */
#define NC_INIT_AGENT (NC_INIT_NOTIF | NC_INIT_MONITORING | NC_INIT_WD | NC_INIT_SINGLELAYER)

/**
 * Environment variabe with settings for verbose level
 */
#define ENVIRONMENT_VERBOSE "NETOPEER_VERBOSE"

/* Define max string size + 1 of session id */
/* to hold "restconf-dummy-[pid of 5 numbers][5 number buffer] */
#define RC_SID_SIZE 26

/* Buffer size when receiving RESTCONF messages*/
#define RC_MSG_BUFF_SIZE 500
#define RC_MSG_BUFF_SIZE_MAX 500

volatile int done = 0;

typedef int model_t;

struct ntf_thread_config {
	struct nc_session *session;
	nc_rpc *subscribe_rpc;
};

// RESTCONF FUNCTIONS - START
NC_MSG_TYPE rc_recv_msg(struct pollfd fds, int infd, int outfd, nc_rpc** rpc, conn_t* con);
int rc_handle_msg(httpmsg* msg, nc_rpc** rpc, int outfd, conn_t* con);
void rc_send_error(int status, int fd);
void save(const char* str, const char* file);

void rc_send_version_response(httpmsg* msg, int outfd);
int rc_send_reply(int outfd/*, nc_reply* reply*/, char* json_dump);
int rc_send_auto_response(int outfd, httpmsg* msg, conn_t* con);
json_t* create_module_json_obj(char* cpblt, int with_schema, conn_t* con);
char* jump_resource_identifier(char* identifier, int times);
// RESTCONF FUNCTIONS - END

/*!
 * \brief Signal handler
 *
 * Handles received UNIX signals and sets value to control main loop
 *
 * \param sig 	signal number
 */
void signal_handler(int sig)
{
	clb_print(NC_VERB_VERBOSE, "Signal received.");

	fprintf(stderr, "Signal %d received.\n", sig);

	switch (sig) {
	case SIGINT:
	case SIGTERM:
	case SIGQUIT:
	case SIGABRT:
	case SIGKILL:
		if (done == 0) {
			/* first attempt */
			done = 1;
		} else {
			/* second attempt */
			clb_print(NC_VERB_ERROR, "Hey! I need some time to stop, be patient next time!");
			exit(EXIT_FAILURE);
		}
		break;
	default:
		clb_print(NC_VERB_ERROR, "exiting on signal.");
		exit(EXIT_FAILURE);
		break;
	}
}

#ifdef ENABLE_TLS
char *get_tls_username(conn_t* conn)
{
	int i, args_len;
	char* hash_env, *san_env = NULL, *starti, *arg = NULL, *subj_env;
	char** args;

	/* parse certificate hashes */
	args_len = 6;
	args = calloc(args_len, sizeof(char*));

	hash_env = getenv("SSL_CLIENT_MD5");
	if (hash_env == NULL) {
		/* nothing we can do */
		goto cleanup;
	}
	args[0] = malloc(3+strlen(hash_env)+1);
	sprintf(args[0], "01:%s", hash_env);

	hash_env = getenv("SSL_CLIENT_SHA1");
	if (hash_env == NULL) {
		goto cleanup;
	}
	args[1] = malloc(3+strlen(hash_env)+1);
	sprintf(args[1], "02:%s", hash_env);

	hash_env = getenv("SSL_CLIENT_SHA256");
	if (hash_env == NULL) {
		goto cleanup;
	}
	args[2] = malloc(3+strlen(hash_env)+1);
	sprintf(args[2], "03:%s", hash_env);

	hash_env = getenv("SSL_CLIENT_SHA256");
	if (hash_env == NULL) {
		goto cleanup;
	}
	args[3] = malloc(3+strlen(hash_env)+1);
	sprintf(args[3], "04:%s", hash_env);

	hash_env = getenv("SSL_CLIENT_SHA384");
	if (hash_env == NULL) {
		goto cleanup;
	}
	args[4] = malloc(3+strlen(hash_env)+1);
	sprintf(args[4], "05:%s", hash_env);

	hash_env = getenv("SSL_CLIENT_SHA512");
	if (hash_env == NULL) {
		goto cleanup;
	}
	args[5] = malloc(3+strlen(hash_env)+1);
	sprintf(args[5], "06:%s", hash_env);

	/* parse SubjectAltName values */
	san_env = getenv("SSL_CLIENT_SAN");
	if (san_env != NULL) {
		san_env = strdup(san_env);
		arg = strtok(san_env, "/");
		while (arg != NULL) {
			++args_len;
			args = realloc(args, args_len*sizeof(char*));
			args[args_len-1] = arg;
			arg = strtok(NULL, "/");
		}
	}

	/* parse commonName */
	subj_env = getenv("SSL_CLIENT_DN");
	if (subj_env != NULL && (starti = strstr(subj_env, "CN=")) != NULL) {
		/* detect if the CN is followed by another item */
		arg = strchr(starti, '/');
		/* get the length of the CN value */
		if (arg != NULL) {
			i = arg - starti;
			arg = NULL;
		} else {
			i = strlen(starti);
		}
		/* store "CN=<value>" into the resulting string */
		++args_len;
		args = realloc(args, args_len*sizeof(char*));
		args[args_len-1] = alloca(i+1);
		strncpy(args[args_len-1], starti, i);
		args[args_len-1][i+1] = '\0';
	}

	arg = comm_cert_to_name(conn, args, args_len);

cleanup:
	for (i = 0; i < 6; ++i) {
		if (args[i] != NULL) {
			free(args[i]);
		}
	}
	free(args);
	if (san_env != NULL) {
		free(san_env);
	}

	return arg;
}

#endif /* ENABLE_TLS */

static void print_usage (char * progname)
{
	fprintf(stdout, "This program is not meant for manual use, it should be\n");
	fprintf(stdout, "started automatically as an SSH Subsystem by an SSH daemon.\n\n");
	fprintf(stdout, "Usage: %s [-h] [-v level]\n", progname);
	fprintf(stdout, " -h                  display help\n");
	fprintf(stdout, " -v level            verbose output level\n");
	exit (0);
}

/* TODO */ extern char **environ;

int main (int argc, char** argv)
{
	conn_t *con;							/* connection to NETCONF server */
	char* nc_session_id;					/* session if of NETCONF session */
	int infd, outfd;						/* file descriptors for receiving RESTCONF messages and sending RESTCONF messages */
	nc_rpc * rpc = NULL;					/* RPC message to server once it has been converted from RESTCONF */

	int ret;
	NC_MSG_TYPE msg_type;
	int timeout = 500;						/* ms, poll timeout */
	struct pollfd fds;
	struct sigaction action;
	int next_option;
	int verbose;
	char *aux_string = NULL;

	/* TODO remove this */
	char **var;
	for (var = environ; *var != NULL; ++var) {
		clb_print(NC_VERB_DEBUG, *var);
	}
	/* TODO until here */

	infd = STDIN_FILENO;
	outfd = STDOUT_FILENO;

	if ((aux_string = getenv(ENVIRONMENT_VERBOSE)) == NULL) {
		verbose = NC_VERB_ERROR;
	} else {
		verbose = atoi(aux_string);
	}

	while ((next_option = getopt(argc, argv, "hv:")) != -1) {
		switch (next_option) {
		case 'h':
			print_usage(argv[0]);
			break;
		case 'v':
			verbose = atoi(optarg);
			break;
		default:
			print_usage(argv[0]);
			break;
		}
	}

	/* set signal handler */
	sigfillset(&action.sa_mask);
	action.sa_handler = signal_handler;
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGQUIT, &action, NULL);
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGKILL, &action, NULL);

	openlog("restconf-agent", LOG_PID, LOG_DAEMON);
	nc_callback_print(clb_print);

	/* normalize value if not from the enum */
	if (verbose < NC_VERB_ERROR) {
		nc_verbosity(NC_VERB_ERROR);
	} else if (verbose > NC_VERB_DEBUG) {
		nc_verbosity(NC_VERB_DEBUG);
	} else {
		nc_verbosity(verbose);
	}

	/* initialize library */
	if (nc_init(NC_INIT_AGENT) < 0) {
		clb_print(NC_VERB_ERROR, "Library initialization failed");
		return EXIT_FAILURE;
	}

	/* connect to server */
	if ((con = comm_connect()) == NULL) {
		clb_print(NC_VERB_ERROR, "Cannot connect to Netopeer server.");
		return EXIT_FAILURE;
	}
	clb_print(NC_VERB_VERBOSE, "Connected with Netopeer server");

#ifndef ENABLE_TLS

	/* restconf expects stunnel or some other TLS transport */
	clb_print(NC_VERB_ERROR, "Restconf agent expects TLS to be enabled.");
	return EXIT_FAILURE;

#endif

	if (!getenv("SSL_CLIENT_DN")) {
		clb_print(NC_VERB_ERROR, "Restconf agent expects SSL_CLIENT_DN environment variable to be set.");
		return EXIT_FAILURE;
	}

	/* create session id */
	nc_session_id = malloc(sizeof(char) * RC_SID_SIZE);

	if (nc_session_id == NULL) {
		clb_print(NC_VERB_ERROR, "Unable to allocate memory for restconf session id.");
		return EXIT_FAILURE;
	}

	memset(nc_session_id, 0, RC_SID_SIZE - 1);
	snprintf(nc_session_id, RC_SID_SIZE - 1, "rc-dummy-%d", getpid());

	/* send information about our would be session to the server */
	struct nc_cpblts* capabilities = nc_session_get_cpblts_default();

	nc_cpblts_add(capabilities, "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring");

	if (comm_session_info_send(con, "root", nc_session_id, nc_cpblts_count(capabilities), capabilities)) {
		clb_print(NC_VERB_ERROR, "Failed to communicate with server.");
		nc_cpblts_free(capabilities);
		return EXIT_FAILURE;
	}

	struct nc_session* netconf_con_dummy = nc_session_dummy(nc_session_id, "root", NULL, capabilities);
	if (netconf_con_dummy == NULL) {
		clb_print(NC_VERB_ERROR, "Could not get dummy netconf session");
		nc_cpblts_free(capabilities);
		return EXIT_FAILURE;
	}

	clb_print(NC_VERB_VERBOSE, "Handshake finished");

	fds.fd = infd;
	fds.events = POLLIN;

	while (!done) {
		ret = poll(&fds, 1, timeout);
		if (ret < 0 && errno != EINTR) { /* poll error */
			clb_print(NC_VERB_ERROR, "poll failed.");
			goto cleanup;
		} else if (ret == 0) { /* timeout */
			continue;
		} else if (ret > 0) { /* event occured */
			if (fds.revents & POLLHUP) { /* client hung up */
				clb_print(NC_VERB_VERBOSE, "Connection closed by client");
				comm_close(con);
				goto cleanup;
			} else if (fds.revents & POLLERR) { /* I/O error */
				clb_print(NC_VERB_ERROR, "I/O error.");
				goto cleanup;
			} else if (fds.revents & POLLIN) { /* data ready */
				/* read data from input */
				clb_print(NC_VERB_DEBUG, "Reading message from client.");
				msg_type = rc_recv_msg(fds, infd, outfd, &rpc, con);
				switch (msg_type) {
				case NC_MSG_NONE:
					clb_print(NC_VERB_VERBOSE, "A message has been processed without sending to the server");
					/* TODO: optionally, log the processed message (what the request was, what the response was */
					/* TODO: the netconf-agent doesn't do this by default so that would require some more support */
					break;
				case NC_MSG_UNKNOWN:
					clb_print(NC_VERB_ERROR, "Could not parse clients message");
					/* TODO: optionally, log the message */
					break;
				case NC_MSG_RPC:
					clb_print(NC_VERB_VERBOSE, "Processed client message");
//
//					clb_print(NC_VERB_VERBOSE, "Processing client message");
//
//					nc_reply* reply = comm_operation(con, rpc);
//
//					// TODO: testing
//					char* ietf_system = get_schema("ietf-system", con, "2");
//					save(ietf_system, "ietf-system.log");
//					char* ietf_system_tls_auth = get_schema("ietf-system-tls-auth", con, "3");
//					save(ietf_system_tls_auth, "ietf-system-tls-auth.log");
//					char* ietf_x509_cert_to_name = get_schema("ietf-x509-cert-to-name", con, "4");
//					save(ietf_x509_cert_to_name, "ietf-x509-cert-to-name.log");
//
//					// TODO: extract needed modules dynamically + conversion from capabilities
//
//					// this works, good
//					module* cert_to_name_mod = read_module_from_string(ietf_x509_cert_to_name);
//					module* ietf_system_tls_auth_mod = read_module_from_string_with_groupings(ietf_system_tls_auth, cert_to_name_mod);
//					module* ietf_system_mod = read_module_from_string(ietf_system);
//
//					xmlDocPtr doc = xmlParseDoc((const xmlChar*)nc_reply_dump(reply));
//					if (doc == NULL) {
//						clb_print(NC_VERB_ERROR, "Message processing failed: could not read xml doc");
//						break;
//					}
//					xmlNodePtr root = xmlDocGetRootElement(doc);
//					if (root == NULL) {
//						clb_print(NC_VERB_ERROR, "Message processing failed: could not get root element");
//						break;
//					}
//					xmlNodePtr data = root->xmlChildrenNode;
//					while (data != NULL && strcmp((char*)data->name, "data")) {
//						clb_print(NC_VERB_WARNING, "Node name is not data, it is:");
//						clb_print(NC_VERB_WARNING, (char*) data->name);
//						data = data->next;
//					}
//					xmlBufferPtr buffer = xmlBufferCreate();
//					xmlNodeDump(buffer, doc, data, 0, 1);
//					save((char*)buffer->content, "data-dump");
//					xmlBufferFree(buffer);
//
//					xmlNodePtr system = data->xmlChildrenNode;
//					while (system != NULL && strcmp((char*) system->name, "system")) {
//						clb_print(NC_VERB_WARNING, "Node name is not system, it is:");
//						clb_print(NC_VERB_WARNING, (char*) system->name);
//						system = system->next;
//					}
//
//					xmlBufferPtr buffer2 = xmlBufferCreate();
//					xmlNodeDump(buffer2, doc, system, 0, 1);
//					save((char*)buffer2->content, "system-dump");
//
//					// convert
//					path* p = new_path(5000);
//					json_t* json_obj = xml_to_json(system, p, ietf_system_mod, ietf_system_tls_auth_mod, NULL, 0, NULL);
//					clb_print(NC_VERB_WARNING, "dumping to json");
//					save(json_dumps(json_obj, 0), "json-dump");
//					free_path(p);
//
//					xmlBufferFree(buffer2);
//
//					destroy_string(ietf_system);
//					destroy_string(ietf_system_tls_auth);
//					destroy_string(ietf_x509_cert_to_name);
//
//					if (reply == NULL) {
//						clb_print(NC_VERB_WARNING, "Message processing failed");
//						rc_send_error(-2, outfd);
//						break;
//					}
//					if (rc_send_reply(outfd/*, reply*/, json_dumps(json_obj, 0))) {
//						clb_print(NC_VERB_WARNING, "Sending reply failed.");
//					}

					break;
				default:
					clb_print(NC_VERB_ERROR, "Illegal state reached, received unsupported NC_MSG_TYPE");
					break;
				}
				goto cleanup;
			}
		}
	}

cleanup:
	comm_close(con);
	nc_cpblts_free(capabilities);
	nc_rpc_free(rpc);
	nc_session_free(netconf_con_dummy);
	nc_close();

	return (EXIT_SUCCESS);
}

// 1, reads HTTP message from client
// 2, parses HTTP message
// 3, sends the message to the handler for further operations

NC_MSG_TYPE rc_recv_msg(struct pollfd fds, int infd, int outfd, nc_rpc** rpc, conn_t* con) {

	clb_print(NC_VERB_VERBOSE, "rc_recv_msg: receiving restconf message");

	int ptr = 0, buffer_size = RC_MSG_BUFF_SIZE;      // ptr - number of bytes read, buffer_size - number of chars in array allowed
	ssize_t rc;                                       // rc - read count - number of bytes read in current iteration
	char* buffer = malloc(buffer_size);

	if (buffer == NULL) {
		clb_print(NC_VERB_ERROR, "rc_recv_msg: could not allocate memory for buffer");
		return NC_MSG_UNKNOWN;
	}

	memset(buffer, 0, buffer_size);

	while (fds.revents & POLLIN) {
		rc = read(infd, buffer + ptr, (buffer_size - ptr) - 1);

		if (rc <= 0) {
			break;
		}

		ptr += rc;

		if (ptr >= buffer_size - 1) {
			// we are out of space and need to extend the buffer
			buffer_size += buffer_size;
			clb_print(NC_VERB_DEBUG, "rc_recv_msg: extending buffer size for HTTP message");
			char* tmp_buffer = realloc(buffer, buffer_size);
			if (tmp_buffer == NULL) {
				clb_print(NC_VERB_ERROR, "rc_recv_msg: could not allocate memory for buffer");
				free(buffer);
				return NC_MSG_UNKNOWN;
			}
			buffer = tmp_buffer;
			memset(buffer + ptr, 0, buffer_size - ptr);
		}

		poll(&fds, 1, 0);
	}

	if (ptr <= 0) {
		clb_print(NC_VERB_ERROR, "rc_recv_msg: received no message, illegal agent state");
		free(buffer);
		return NC_MSG_UNKNOWN;
	} else {
		clb_print(NC_VERB_VERBOSE, "rc_recv_msg: received restconf message");
	}

	clb_print(NC_VERB_DEBUG, "rc_recv_msg: parsing restconf message");
	httpmsg* msg = parse_req(buffer);
	free(buffer);
	if (msg == NULL) {
		// allocation error
		return NC_MSG_UNKNOWN;
	}

	clb_print(NC_VERB_DEBUG, "rc_recv_msg: handling restconf message");
	int status = rc_handle_msg(msg, rpc, outfd, con);
	if (status < 0) {
		clb_print(NC_VERB_ERROR, "rc_recv_msg: an error has occurred while handling restconf message");
		httpmsg_clean(msg);
		return NC_MSG_UNKNOWN;
	}

	clb_print(NC_VERB_DEBUG, "rc_recv_msg: finished handling restconf message");
	return NC_MSG_RPC;
}

// checks /restconf part of resource identifier
// checks validity of the next part of resource identifier
// handles the message based on the rest of the resource identifier and given method, delegates to other functions
// either creates rpc or communicates with client independently
// returns 0 if ok
// returns -1 on communication error
int rc_handle_msg(httpmsg* msg, nc_rpc** rpc, int outfd, conn_t* con) {
	char* restconf_resource = msg->resource_locator;
	if (strncmp(restconf_resource, "/restconf", strlen("/restconf"))) {
		rc_send_error(-3, outfd);
		return 0; // correctly processed
	}
	if (!strcmp(restconf_resource, "/restconf") ||
			!strcmp(restconf_resource, "/restconf/") ) {
//		rc_handle_pure_restconf(msg, outfd, con);
		return 0;
	}

	char* subresource = msg->resource_locator + strlen("/restconf");
	if (!strncmp(subresource, "/data", strlen("/data"))) {
		clb_print(NC_VERB_DEBUG, "rc_handle_msg: processing /data branch");
//		if (!strcmp()) {
//
//		} else {
//			rc_send_error(-3, outfd);
//		}
		return 0;
	} else if (!strncmp(subresource, "/modules", strlen("/modules"))) {
		clb_print(NC_VERB_DEBUG, "rc_handle_msg: processing /modules branch");
		return rc_send_auto_response(outfd, msg, con);
	} else if (!strncmp(subresource, "/version", strlen("/version"))) {
		clb_print(NC_VERB_DEBUG, "rc_handle_msg: processing /version branch");
		rc_send_version_response(msg, outfd);
		return 0;
	} else if (!strncmp(subresource, "/streams", strlen("/streams"))) {
		clb_print(NC_VERB_DEBUG, "rc_handle_msg: processing /streams branch");
		rc_send_error(-1, outfd);
		return 0;
	} else if (!strncmp(subresource, "/operations", strlen("/operations"))) {
		clb_print(NC_VERB_DEBUG, "rc_handle_msg: processing /operations branch");
		rc_send_error(-1, outfd);
		return 0;
	}

	rc_send_error(-3, outfd); // unknown /restconf subresource
	return 0;
}

void rc_send_version_response(httpmsg* msg, int outfd) {

	if (strcmp(msg->method, "GET")) {
		clb_print(NC_VERB_DEBUG, "rc_send_auto_response: received bad method");
		rc_send_error(-3, outfd); // Bad Request, no such method is supported on the /version resource
		return;
	}

	int hdr_c = 0;
	for (hdr_c = 0; hdr_c < msg->header_num; hdr_c++) {
		if (!strcmp(msg->headers[hdr_c], "Accept: application/yang.api+json") ||
				!strcmp(msg->headers[hdr_c], "Accept: */*")) {
			break;
		}
		if (hdr_c + 1 == msg->header_num) {
			clb_print(NC_VERB_DEBUG, "rc_send_auto_response: received request on /modules without proper Accept header");
			rc_send_error(-3, outfd);
			return;
		}
	}

	if (strcmp(msg->resource_locator, "/restconf/version") &&
			strcmp(msg->resource_locator, "/restconf/version/")) {
		rc_send_error(-3, outfd);
		return;
	}

	json_t* version = json_object();
	json_object_set(version, "version", json_string("1.0"));

	char* dump = json_dumps(version, JSON_INDENT(2));
	rc_send_reply(outfd, dump);
	free(dump);
	json_decref(version);
	return;
}

void rc_send_error(int status, int fd) {
	switch(status) {
	case -1:
	{
		char string[50];
		memset(string, 0, 50);
		snprintf(string, 49, "HTTP/1.1 501 Not Implemented\r\n"
				"\r\n\r\n");
		int count = write(fd, string, strlen(string));
		if (count < 0) {
			clb_print(NC_VERB_ERROR, "Write failed.");
		}
		break;
	}
	case -2:
	{
		char string[50];
		memset(string, 0, 50);
		snprintf(string, 49, "HTTP/1.1 500 Internal Error\r\n"
				"\r\n\r\n");
		int count = write(fd, string, strlen(string));
		if (count < 0) {
			clb_print(NC_VERB_ERROR, "Write failed.");
		}
		break;
	}
	case -3:
	{
		char string[50];
		memset(string, 0, 50);
		snprintf(string, 49, "HTTP/1.1 400 Bad Request\r\n"
				"\r\n\r\n");
		int count = write(fd, string, strlen(string));
		if (count < 0) {
			clb_print(NC_VERB_ERROR, "Write failed.");
		}
		break;
	}
	default:
		clb_print(NC_VERB_ERROR, "rc_send_error: received unknown status");
		break;
	}
}

int rc_send_reply(int outfd, char* json_dump) {
	clb_print(NC_VERB_VERBOSE, "Replying to client");
	/* prepare message */
	int payload_size = strlen(json_dump);
	char* status_line = "HTTP/1.1 200 OK\r\n";
	char* headers = malloc(50); /* enough to hold "Content-Length: <number>\r\n\r\n" */
	if (headers == NULL) {
		clb_print(NC_VERB_ERROR, "Could not allocate memory for headers");
		free(json_dump);
		return EXIT_FAILURE;
	}
	memset(headers, 0, 50);
	int ret = snprintf(headers, 49, "Content-Length: %d\r\n\r\n", payload_size);
	if (ret < 0) {
		clb_print(NC_VERB_ERROR, "Could not print headers");
		free(headers);
		free(json_dump);
		return EXIT_FAILURE;
	}
	int message_size = strlen(status_line) + strlen(headers) + payload_size + 1;
	char* message = malloc(message_size + 1);
	if (message == NULL) {
		clb_print(NC_VERB_ERROR, "Could not allocate memory for message");
		free(headers);
		free(json_dump);
		return EXIT_FAILURE;
	}
	memset(message, 0, message_size + 1);
	ret = snprintf(message, message_size, "%s%s%s", status_line, headers, json_dump);
	if (ret < 0) {
		clb_print(NC_VERB_ERROR, "Could not print message");
		free(headers);
		free(message);
		free(json_dump);
		return EXIT_FAILURE;
	}

	/* send message */
	int count = write(outfd, message, message_size);
	close(outfd);
	free(headers);
	free(message);

	if (count < 0) {
		clb_print(NC_VERB_ERROR, "Writing message failed.");
		free(json_dump);
		return EXIT_FAILURE;
	} else {
		clb_print(NC_VERB_ERROR, "Writing response was successful");
	}

	free(json_dump);
	return EXIT_SUCCESS;
}

json_t* create_module_json_obj(char* cpblt, int with_schema, conn_t* con) {
	clb_print(NC_VERB_DEBUG, "create_module_json_obj: started retrieving from capability:");
	clb_print(NC_VERB_DEBUG, cpblt);
	json_t* obj = json_object();
	char* module_id = strchr(cpblt, '?') == NULL ? NULL : strchr(cpblt, '?') + 1;
	if (module_id == NULL || (strncmp("module", module_id, 6))) {
		return NULL; // this is not a module declaration
	}
	clb_print(NC_VERB_DEBUG, "create_module_json_obj: creating copy");
	char* cpblt_copy = malloc(strlen(cpblt) + 1);
	memset(cpblt_copy, 0, strlen(cpblt) + 1);
	strncpy(cpblt_copy, cpblt, strlen(cpblt));

	clb_print(NC_VERB_DEBUG, "create_module_json_obj: setting namespace");
	// set namespace
	char* delim = strchr(cpblt_copy, '?');
	if (delim != NULL) {
		delim[0] = '\0';
		json_object_set(obj, "namespace", json_string(cpblt_copy));
		delim[0] = '?';
	}

	clb_print(NC_VERB_DEBUG, "create_module_json_obj: setting name");
	// set name
	delim = strstr(cpblt_copy, "module=") + 7;
	char* end_del = strchr(delim, '&');
	if (end_del == NULL) end_del = strchr(delim, ';');
	if (delim != NULL) {
		if (end_del != NULL) end_del[0] = '\0';
		json_object_set(obj, "name", json_string(delim));
		if (end_del != NULL) end_del[0] = '&';
	}

	clb_print(NC_VERB_DEBUG, "create_module_json_obj: setting revision");
	// set revision
	delim = strstr(cpblt_copy, "revision=") + 9;
	end_del = strchr(delim, '&');
	if (end_del == NULL) end_del = strchr(delim, ';');
	if (delim != NULL) {
		if (end_del != NULL) end_del[0] = '\0';
		json_object_set(obj, "revision", json_string(delim));
		if (end_del != NULL) end_del[0] = '&';
	}

	clb_print(NC_VERB_DEBUG, "create_module_json_obj: setting features");
	// set features
	json_object_set(obj, "features", json_array());
	delim = strstr(cpblt_copy, "features=") == NULL ? NULL : strstr(cpblt_copy, "features=") + 9;
	if (delim != NULL) {
		char* features = delim;
		end_del = strchr(features, ',');
		do {
			if (end_del != NULL) end_del[0] = '\0';
			json_array_append(json_object_get(obj, "features"), json_string(features));
			if (end_del != NULL) end_del[0] = ',';
			features = strchr(features, ',') == NULL ? NULL : strchr(features, ',') + 1;
			end_del = strchr(features, ',');
		} while (features != NULL);
	}

	// set schema
	if (with_schema) {
		clb_print(NC_VERB_DEBUG, "create_module_json_obj: setting schema");
		char* schema = get_schema(json_string_value(json_object_get(obj, "name")), con, "1"); // TODO: correct message id - unified message id setup for all communication with the server
		json_object_set(obj, "schema", json_string(schema));
		free(schema);
	}

	clb_print(NC_VERB_DEBUG, "create_module_json_obj: freeing capability copy and returning json object");
	free(cpblt_copy);
	return obj;
}

char* jump_resource_identifier(char* identifier, int times) {
	if (identifier == NULL || identifier[0] == '\0') {
		clb_print(NC_VERB_WARNING, "Attempting to jump invalid identifier.");
		return identifier;
	}
	char* new_id = identifier;
	int i = 0;
	for (i = 0; i < times; i++) {
		char* tmp_id = strchr(new_id, '/');
		if (tmp_id == NULL) {
			break;
		}
		new_id = tmp_id + 1;
	}
	return new_id;
}

/* creates response based on msg resource locator - serves /modules resources */
int rc_send_auto_response(int outfd, httpmsg* msg, conn_t* con) {
	clb_print(NC_VERB_DEBUG, "rc_send_auto_response: sending response based on resource locator");

	if (!strcmp(msg->method, "POST") || !strcmp(msg->method, "PUT") || !strcmp(msg->method, "PATCH")
			|| !strcmp(msg->method, "OPTIONS") || !strcmp(msg->method, "DELETE") || !strcmp(msg->method, "HEAD")) {
		clb_print(NC_VERB_DEBUG, "rc_send_auto_response: received unimplemented method:");
		clb_print(NC_VERB_DEBUG, msg->method);
		rc_send_error(-1, outfd); // Not implemented
		return 0;
	}

	if (strcmp(msg->method, "GET")) {
		clb_print(NC_VERB_DEBUG, "rc_send_auto_response: received bad method");
		rc_send_error(-3, outfd); // Bad Request, this method is not supported by RESTCONF at all
		return 0;
	}

	int hdr_c = 0;
	for (hdr_c = 0; hdr_c < msg->header_num; hdr_c++) {
		if (!strcmp(msg->headers[hdr_c], "Accept: application/yang") ||
				!strcmp(msg->headers[hdr_c], "Accept: application/yang.api+json") ||
				!strcmp(msg->headers[hdr_c], "Accept: */*")) {
			break;
		}
		if (hdr_c + 1 == msg->header_num) {
			clb_print(NC_VERB_DEBUG, "rc_send_auto_response: received request on /modules without proper Accept header");
			rc_send_error(-3, outfd);
			return 0;
		}
	}

	clb_print(NC_VERB_DEBUG, "rc_send_auto_response: getting server capabilities");
	char** cpblts = comm_get_srv_cpblts(con);
	if (cpblts == NULL) {
		// some internal error occurred
		return -1;
	}

	int i = 0;

	json_t* modules_obj = json_object();
	json_object_set(modules_obj, "ietf-restconf:modules", json_array());

	clb_print(NC_VERB_DEBUG, "rc_send_auto_response: testing for schema only request");
	char* module_id = jump_resource_identifier(msg->resource_locator, 3);
	if (!strncmp(module_id, "module", 6)) {
		char* schema_id = jump_resource_identifier(msg->resource_locator, 5); // /restconf/modules/module/<mod>/schema
		clb_print(NC_VERB_DEBUG, schema_id);
		if (!strncmp(schema_id, "schema", 6)) {
			char* module_id = jump_resource_identifier(msg->resource_locator, 4);
			if (NULL != strchr(module_id, '/')) {
				strchr(module_id, '/')[0] = '\0';
			}
			if (NULL != strchr(module_id, '&')) {
				strchr(module_id, '&')[0] = '\0';
			}
			if (NULL != strchr(module_id, '#')) {
				strchr(module_id, '#')[0] = '\0';
			}
			clb_print(NC_VERB_VERBOSE, "rc_send_auto_response: getting schema");
			clb_print(NC_VERB_DEBUG, module_id);
			char* schema = get_schema(module_id, con, "200");
			clb_print(NC_VERB_VERBOSE, "rc_send_auto_response: got schema, sending reply");
			rc_send_reply(outfd, schema);
			return 0;
		}
	}

	i = 0;
	while (cpblts[i] != NULL) {
		clb_print(NC_VERB_VERBOSE, "rc_send_auto_response: constructing module JSON structure for capability:");
		clb_print(NC_VERB_VERBOSE, cpblts[i]);
		json_t* module = create_module_json_obj(cpblts[i], 1, con);
		if (module != NULL) {
			clb_print(NC_VERB_DEBUG, "rc_send_auto_response: appending module");
			json_array_append(json_object_get(modules_obj, "ietf-restconf:modules"), module);
		}
		i++;
	}

	char* dump_2 = json_dumps(modules_obj, JSON_INDENT(2));

	clb_print(NC_VERB_DEBUG, "rc_send_auto_response: sending modules dump");
	if (!strncmp(msg->resource_locator, "/restconf/modules", strlen("/restconf/modules"))) {
		clb_print(NC_VERB_DEBUG, "rc_send_auto_response: resource locator start is valid");
		if (!strcmp(msg->resource_locator, "/restconf/modules/module") ||
				!strcmp(msg->resource_locator, "/restconf/modules/module/") ||
				!strcmp(msg->resource_locator, "/restconf/modules") ||
				!strcmp(msg->resource_locator, "/restconf/modules/")) {
			clb_print(NC_VERB_DEBUG, "rc_send_auto_response: sending whole dump");
			// send whole dump
			rc_send_reply(outfd, dump_2);
		} else {
			if (strncmp(msg->resource_locator, "/restconf/modules/module/", strlen("/restconf/modules/module/"))) { // wrong resource locator value
				clb_print(NC_VERB_DEBUG, "rc_send_auto_response: wrong resource locator value");
				free(dump_2);
				clb_print(NC_VERB_DEBUG, "rc_send_auto_response: freeing capabilities");
				i = 0;
				while (cpblts[i] != NULL) {
					free(cpblts[i]);
					i++;
				}
				rc_send_error(-3, outfd);
				return -2; // unknown resource locator value
			}
			clb_print(NC_VERB_DEBUG, "rc_send_auto_response: parsing module name");
			char* module_id = msg->resource_locator + strlen("/restconf/modules/module/");
			unsigned int i = 0;
			for (i = 0; i < json_array_size(json_object_get(modules_obj, "ietf-restconf:modules")); i++) {
				json_t* module = json_array_get(json_object_get(modules_obj, "ietf-restconf:modules"), i);
				char* end_del = strstr(module_id, "%20") == NULL ? NULL : strstr(module_id, "%20");
				if (end_del != NULL) end_del[0] = '\0';
				if (!strcmp(module_id, json_string_value(json_object_get(module, "name")))) {
					clb_print(NC_VERB_DEBUG, "rc_send_auto_response: found module name");
					char* module_dump = json_dumps(module, JSON_INDENT(2));
					rc_send_reply(outfd, module_dump);
					free(module_dump);
					if (end_del != NULL) end_del[0] = '%';
					break;
				}
				if (end_del != NULL) end_del[0] = '%';
			}
		}
	} else {
		free(dump_2);
		clb_print(NC_VERB_DEBUG, "rc_send_auto_response: freeing capabilities");
		i = 0;
		while (cpblts[i] != NULL) {
			free(cpblts[i]);
			i++;
		}
		rc_send_error(-3, outfd);
		return -2; // unknown resource locator value
	}

	free(dump_2);
	clb_print(NC_VERB_DEBUG, "rc_send_auto_response: freeing capabilities");
	i = 0;
	while (cpblts[i] != NULL) {
		free(cpblts[i]);
		i++;
	}

	rc_send_error(-3, outfd);

	return 0;
}