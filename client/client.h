/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef CLIENT_H
#define CLIENT_H

#include "logs.h"
#include "protocol.h"

struct state {
	// options
	const char *host;
	const char *port;
	// states value
	int socket_fd;
};


/* protocol */
/**
 * send status request
 *
 * @param fd socket to write on
 * @return 0 on success, -errno on error
 */
int protocol_request_status(int fd);


#endif
