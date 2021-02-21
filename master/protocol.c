/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "master_ct.h"

static int status_cb(int fd, json_t *json, void *arg) {
	struct state *state = arg;
	(void)json; // XXX unused, use attribute

	return protocol_reply_status(fd, &state->stats, 0, NULL);
}

protocol_read_cb protocol_cbs[PROTOCOL_COMMANDS_MAX] = {
	[STATUS] = status_cb,
};
