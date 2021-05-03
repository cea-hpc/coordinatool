/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef CLIENT_H
#define CLIENT_H

#include "logs.h"
#include "protocol.h"
#include "utils.h"

struct active_requests_state {
	json_t *hai_list;
	unsigned int archive_id;
	unsigned long flags;
	char fsname[LUSTRE_MAXFSNAME];
};

struct state {
	// options
	struct {
		const char *host;
		const char *port;
		bool send_queue;
		uint32_t max_archive;
		uint32_t max_restore;
		uint32_t max_remove;
		uint32_t hsm_action_list_size;
		uint32_t archive_id;
	} config;
	// states value
	int socket_fd;
	union {
		struct active_requests_state active_requests;
	};
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
int protocol_request_queue(int fd,
			   struct active_requests_state *active_requests);

#endif
