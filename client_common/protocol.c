/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "client_common.h"

#pragma GCC diagnostic ignored "-Wunused-parameter"

int ct_connect(struct ct_state *state, const char *path, int archive_count,
	       int *archives, int rfd_flags) {
	return 0;
}

int ct_disconnect(struct ct_state *state) {
	return 0;
}

int ct_request_recv(struct ct_state *state) {
	return 0;
}

int ct_request_start(struct ct_state *state, unsigned long long cookie,
		     int restore_mdt_index, int restore_open_flags, bool is_error) {
	return 0;
}
int ct_request_done(struct ct_state *state, unsigned long long cookie,
		    const struct hsm_extent *he, int hp_flags, int errval) {
	return 0;
}

/* wraps protocol_read_command but allow parallel polling */
int ct_read_command(struct ct_state *state, void *cbs, void *arg, int *until) {
	return 0;
}
