/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef CLIENT_H
#define CLIENT_H

#include "logs.h"
#include "protocol.h"
#include "utils.h"
#include "client_common.h"

struct active_requests_state {
	json_t *hai_list;
	unsigned int archive_id;
	unsigned long flags;
	char fsname[LUSTRE_MAXFSNAME];
};

struct client {
	struct ct_state state;
	bool send_queue;
	union {
		struct active_requests_state active_requests;
	};
};


/* protocol */
/** reply handlers vector */
extern protocol_read_cb protocol_cbs[];

#endif
