/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef CLIENT_COMMON_H
#define CLIENT_COMMON_H

#include <asm/param.h>

#include "protocol.h"

struct ct_state {
	// options
	struct ct_state_config {
		const char *confpath;
		const char *host;
		const char *port;
		const char *client_id;
		uint32_t max_archive;
		uint32_t max_restore;
		uint32_t max_remove;
		uint32_t hsm_action_list_size;
		enum llapi_message_level verbose;
	} config;
	// state values
	int socket_fd;
	const char *fsname;
	json_t *archive_ids;
};

/* client.c */
int ct_config_init(struct ct_state_config *config);
void ct_free(struct ct_state *state);

/* tcp.c */
/* optionally send running actions on reconnect */
int tcp_connect(struct ct_state *state, json_t *hai_list);

/* protocol.c */

int protocol_checkerror(json_t *json);
/**
 * send status request
 *
 * @param fd socket to write on
 * @return 0 on success, -errno on error
 */
int protocol_archive_ids(int archive_count, int *archives, json_t **out);
int protocol_request_status(const struct ct_state *state, int verbose);
int protocol_request_recv(const struct ct_state *state);
int protocol_request_done(const struct ct_state *state, uint64_t cookie,
			  struct lu_fid *dfid, int status);
int protocol_request_queue(const struct ct_state *state, json_t *hai_list);
int protocol_request_ehlo(const struct ct_state *state, json_t *hai_list);
extern protocol_read_cb protocol_ehlo_cbs[];

#endif
