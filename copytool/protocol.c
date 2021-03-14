/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"

/**************
 *  callbacks *
 **************/

static int status_cb(int fd, json_t *json, void *arg) {
	struct state *state = arg;
	(void)json; // XXX unused, use attribute

	return protocol_reply_status(fd, &state->stats, 0, NULL);
}

protocol_read_cb protocol_cbs[PROTOCOL_COMMANDS_MAX] = {
	[STATUS] = status_cb,
};

/*****************
 * reply helpers *
 *****************/

int protocol_reply_status(int fd, struct ct_stats *ct_stats, int status,
			  char *error) {
	json_t *reply;
	int rc = 0;

	reply = json_object(); // XXX check if can fail
	if (!reply) {
		rc = -ENOMEM;
		LOG_ERROR(rc, "Could not allocate new object");
		return rc;
	}
	if ((rc = protocol_setjson_str(reply, "command", "status")) ||
	    (rc = protocol_setjson_int(reply, "status", status)) ||
	    (rc = protocol_setjson_str(reply, "error", error)) ||
	    (rc = protocol_setjson_int(reply, "running_archive", ct_stats->running_archive)) ||
	    (rc = protocol_setjson_int(reply, "running_restore", ct_stats->running_restore)) ||
	    (rc = protocol_setjson_int(reply, "running_remove", ct_stats->running_remove)) ||
	    (rc = protocol_setjson_int(reply, "pending_archive", ct_stats->pending_archive)) ||
	    (rc = protocol_setjson_int(reply, "pending_restore", ct_stats->pending_restore)) ||
	    (rc = protocol_setjson_int(reply, "pending_remove", ct_stats->pending_remove)) ||
	    (rc = protocol_setjson_int(reply, "done_archive", ct_stats->done_archive)) ||
	    (rc = protocol_setjson_int(reply, "done_restore", ct_stats->done_restore)) ||
	    (rc = protocol_setjson_int(reply, "done_remove", ct_stats->done_remove)) ||
	    (rc = protocol_setjson_int(reply, "clients_connected", ct_stats->clients_connected)))
		goto out_freereply;

	LOG_INFO("Sending reply status to %d: %s", fd, json_dumps(reply, 0));
	if (protocol_write(reply, fd, 0) != 0) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write reply to %d: %s", fd,
			  json_dumps(reply, 0));
		goto out_freereply;
	};

out_freereply:
	json_decref(reply);
	return rc;
}
