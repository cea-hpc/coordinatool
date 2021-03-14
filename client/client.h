/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef CLIENT_H
#define CLIENT_H

#include "logs.h"
#include "protocol.h"
#include "utils.h"

struct state {
	// options
	struct {
		const char *host;
		const char *port;
		uint32_t max_archive;
		uint32_t max_restore;
		uint32_t max_remove;
		uint32_t hsm_action_list_size;
		uint32_t archive_id;
	} config;
	// states value
	int socket_fd;
};


/* protocol */
/** reply handlers vector */
extern protocol_read_cb protocol_cbs[];

/**
 * send status request
 *
 * @param fd socket to write on
 * @return 0 on success, -errno on error
 */
int protocol_request_status(int fd);
int protocol_request_recv(int fd, struct state *state);

#endif
