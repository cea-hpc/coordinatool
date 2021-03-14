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

	LOG_INFO("Sending status request to %d", fd);
	if (json_dumpfd(request, fd, 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write status request to %d", fd);
		goto out_free;
	}

out_free:
	json_decref(request);
	return rc;
}

int protocol_request_recv(int fd, struct state *state) {
	json_t *request;
	int rc = 0;

	request = json_pack("{ss,si,si,si,si,si}",
			    "command", "recv",
			    "max_archive", state->config.max_archive,
			    "max_restore", state->config.max_restore,
			    "max_remove", state->config.max_remove,
			    "max_bytes", state->config.hsm_action_list_size,
			    "archive_id", state->config.archive_id);
	if (!request) {
		rc = -ENOMEM;
		LOG_ERROR(rc, "Could not pack recv request");
		return rc;
	}
	LOG_INFO("Sending recv request to %d", fd);
	if (json_dumpfd(request, fd, 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write recv request to %d", fd);
		goto out_free;
	}

out_free:
	json_decref(request);
	return rc;
}
