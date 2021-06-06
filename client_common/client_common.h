/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef CLIENT_COMMON_H
#define CLIENT_COMMON_H

#include "protocol.h"

struct ct_state {
	int fd;
	// config etc
	// fsname
	// locks etc..
};

int ct_connect(struct ct_state *state, const char *path, int archive_count,
	       int *archives, int rfd_flags);

int ct_disconnect(struct ct_state *state);

int ct_request_recv(struct ct_state *state);
int ct_request_start(struct ct_state *state, unsigned long long cookie,
		     int restore_mdt_index, int restore_open_flags, bool is_error);
int ct_request_done(struct ct_state *state, unsigned long long cookie,
		    const struct hsm_extent *he, int hp_flags, int errval);

/* wraps protocol_read_command but allow parallel polling */
int ct_read_command(struct ct_state *state, void *cbs, void *arg, int *until);

#endif
