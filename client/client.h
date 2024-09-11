/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef CLIENT_H
#define CLIENT_H

#include "logs.h"
#include "protocol.h"
#include "utils.h"
#include "client_common.h"

enum client_mode {
	MODE_STATUS,
	MODE_QUEUE,
	MODE_RECV,
	MODE_DRAIN,
};

struct active_requests_state {
	json_t *hai_list;
	const char *fsname;
};

struct client {
	struct ct_state state;
	int iters;
	enum client_mode mode;
	union {
		// queue
		struct {
			struct active_requests_state active_requests;
			int sent_items;
		};
	};
};


/* protocol */
/** reply handlers vector */
extern protocol_read_cb protocol_cbs[];

#endif
