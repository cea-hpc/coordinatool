/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "client.h"

/**************
 *  callbacks *
 **************/

static int status_cb(int fd, json_t *json, void *arg) {
	(void)json; // XXX unused, use attribute
	(void)arg;
	(void)fd;
	return 0;
}

protocol_read_cb protocol_cbs[PROTOCOL_COMMANDS_MAX] = {
	[STATUS] = status_cb,
};

/*****************
 * reply helpers *
 *****************/

int protocol_request_status(int fd) {
	json_t *request;
	int rc = 0;

	request = json_pack("{ss}", "command", "status");
	if (!request) {
		rc = -ENOMEM;
		LOG_ERROR(rc, "Could not pack status request");
		return rc;
	}

	LOG_INFO("Sending request status to %d", fd);
	if (json_dumpfd(request, fd, 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write status request to %d", fd);
		goto out_free;
	}

out_free:
	json_decref(request);
	return rc;
}
