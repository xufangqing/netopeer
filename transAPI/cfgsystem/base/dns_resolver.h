/**
 * \file dns_resolver.c
 * \brief Functions for DNS resolver configuration
 * \author Michal Vasko <mvasko@cesnet.cz>
 * \date 2013
 *
 * Copyright (C) 2013 CESNET
 *
 * LICENSE TERMS
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
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#ifndef DNS_RESOLVER_H_
#define DNS_RESOLVER_H_

#include <stdbool.h>

xmlNodePtr dns_augeas_getxml(char** msg, xmlNsPtr ns);

/**
 * @brief check the number of search domains for equality
 * @param a augeas structure to use
 * @param search_node "search" node from the configuration
 * @param msg error message in case of an error
 * @return true equal
 * @return false non-equal
 */
bool dns_augeas_equal_search_count(xmlNodePtr search_node, char** msg);

/**
 * @brief add a new search domain to the resolv configuration file
 * @param a augeas structure to use
 * @param domain domain name to be added
 * @param index index of the newly added domain <1; oo)
 * @param msg error message if an error occured
 * @return EXIT_FAILURE error occured
 * @return EXIT_SUCCESS otherwise
 */
int dns_augeas_add_search_domain(const char* domain, int index, char** msg);

/**
 * @brief remove a search domain from the resolv configuration file
 * @param a augeas structure to use
 * @param domain domain name to be removed
 * @param msg error message if an error occured
 * @return EXIT_FAILURE error occured
 * @return EXIT_SUCCESS otherwise
 */
int dns_augeas_rem_search_domain(const char* domain, char** msg);

/**
 * @brief remove all search domains from the resolv configuration file
 * @param a augeas structure to use
 */
void dns_augeas_rem_all_search_domains(void);

/**
 * @brief add a new nameserver to the resolv configuration file
 * @param a augeas structure to use
 * @param address nameserver address
 * @param index index of the newly added nameserver <1; oo)
 * @param msg error message if an error occured
 * @return EXIT_FAILURE error occured
 * @return EXIT_SUCCESS otherwise
 */
int dns_augeas_add_nameserver(const char* address, int index, char** msg);

/**
 * @brief remove a nameserver from the resolv configuration file
 * @param a augeas structure to use
 * @param address nameserver address
 * @param msg error message if an error occured
 * @return EXIT_FAILURE error occured
 * @return EXIT_SUCCESS otherwise
 */
int dns_augeas_rem_nameserver(const char* address, char** msg);

/**
 * @brief check the number of nameservers for equality
 * @param a augeas structure to use
 * @param address_node "server" node from the configuration
 * @param msg error message in case of an error
 * @return true equal
 * @return false non-equal
 */
bool dns_augeas_equal_nameserver_count(xmlNodePtr address_node, char** msg);

/**
 * @brief remove all nameservers from the resolv configuration file
 * @param a augeas structure to use
 */
void dns_augeas_rem_all_nameservers(void);

/**
 * @brief add the timeout option to the resolv configuration file
 * @param a augeas structure to use
 * @param number timeout value
 * @param msg error message if an error occured
 * @return EXIT_FAILURE error occured
 * @return EXIT_SUCCESS otherwise
 */
int dns_augeas_add_opt_timeout(const char* number, char** msg);

/**
 * @brief remove the timeout option from the resolv configuration file
 * @param a augeas structure to use
 * @param number timeout value
 * @param msg error message if an error occured
 * @return EXIT_FAILURE error occured
 * @return EXIT_SUCCESS otherwise
 */
int dns_augeas_rem_opt_timeout(const char* number, char** msg);

/**
 * @brief modify the timeout option in the resolv configuration file
 * @param a augeas structure to use
 * @param number new timeout value
 * @param msg error message if an error occured
 * @return EXIT_FAILURE error occured
 * @return EXIT_SUCCESS otherwise
 */
int dns_augeas_mod_opt_timeout(const char* number, char** msg);

/**
 * @brief add the attempts option to the resolv configuration file
 * @param a augeas structure to use
 * @param number attempts value
 * @param msg error message if an error occured
 * @return EXIT_FAILURE error occured
 * @return EXIT_SUCCESS otherwise
 */
int dns_augeas_add_opt_attempts(const char* number, char** msg);

/**
 * @brief remove the attempts option from the resolv configuration file
 * @param a augeas structure to use
 * @param number attempts value
 * @param msg error message if an error occured
 * @return EXIT_FAILURE error occured
 * @return EXIT_SUCCESS otherwise
 */
int dns_augeas_rem_opt_attempts(const char* number, char** msg);

/**
 * @brief modify the attempts option in the resolv configuration file
 * @param a augeas structure to use
 * @param number new attempts value
 * @param msg error message if an error occured
 * @return EXIT_FAILURE error occured
 * @return EXIT_SUCCESS otherwise
 */
int dns_augeas_mod_opt_attempts(const char* number, char** msg);

#endif /* DNS_RESOLVER_H_ */