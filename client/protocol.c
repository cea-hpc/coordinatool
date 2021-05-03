/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "client.h"

/**************
 *  callbacks *
 **************/

static int status_cb(int fd UNUSED, json_t *json, void *arg UNUSED) {
	printf("Got status reply:\n");
	protocol_write(json, STDOUT_FILENO, JSON_INDENT(2));
	printf("\n");
	return 0;
}

static int recv_cb(int fd UNUSED, json_t *json, void *arg UNUSED) {
	printf("Got recv reply:\n");
	protocol_write(json, STDOUT_FILENO, JSON_INDENT(2));
	printf("\n");
	return 0;
}

static int queue_cb(int fd UNUSED, json_t *json, void *arg UNUSED) {
	printf("Got recv reply:\n");
	protocol_write(json, STDOUT_FILENO, JSON_INDENT(2));
	printf("\n");
	return 0;
}

protocol_read_cb protocol_cbs[PROTOCOL_COMMANDS_MAX] = {
	[STATUS] = status_cb,
	[RECV] = recv_cb,
	[QUEUE] = queue_cb,
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
	if (protocol_write(request, fd, 0)) {
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
	if (protocol_write(request, fd, 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write recv request to %d", fd);
		goto out_free;
	}

out_free:
	json_decref(request);
	return rc;
}

int protocol_request_queue(int fd,
			   struct active_requests_state *active_requests) {
	json_t *hai_list = active_requests->hai_list;
	int rc;

	json_t *hal = json_object();
	if (!hal)
		abort();
	if ((rc = protocol_setjson_int(hal, "hal_version", HAL_VERSION)) ||
	    (rc = protocol_setjson_int(hal, "hal_count", json_array_size(hai_list))) ||
	    (rc = protocol_setjson_int(hal, "hal_archive_id", active_requests->archive_id)) ||
	    (rc = protocol_setjson_int(hal, "hal_flags", active_requests->flags)) ||
	    (rc = protocol_setjson_str(hal, "hal_fsname", active_requests->fsname))) {
		LOG_ERROR(rc, "Could not fill hsm action list");
		json_decref(hal);
		return rc;
	}
	if ((rc = protocol_setjson(hal, "list", hai_list)))
		return rc;

	json_t *request = json_object();
	if (!request)
		abort();

	if ((rc = protocol_setjson(request, "hsm_action_list", hal)) ||
	    (rc = protocol_setjson_str(request, "command", "queue")))
		goto out_free;

	if (protocol_write(request, fd, 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write queue request to %d", fd);
		goto out_free;
	}
out_free:
	json_decref(request);
	return rc;
}
