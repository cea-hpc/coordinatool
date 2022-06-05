/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "client_common.h"
#include "utils.h"


int protocol_checkerror(json_t *reply) {
	int rc = protocol_getjson_int(reply, "status", 0);
	if (rc) {
		const char *error = protocol_getjson_str(reply, "error",
							 NULL, NULL);
		LOG_ERROR(rc, "error: %s",
			  error ? error : "(no detail)");
	}
	return rc;
}

int protocol_request_status(const struct ct_state *state) {
	json_t *request;
	int rc = 0;

	request = json_pack("{ss}", "command", "status");
	if (!request) {
		rc = -ENOMEM;
		LOG_ERROR(rc, "Could not pack status request");
		return rc;
	}

	LOG_INFO("Sending status request to %d", state->socket_fd);
	if (protocol_write(request, state->socket_fd, "status", 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write status request");
		goto out_free;
	}

out_free:
	json_decref(request);
	return rc;
}

int protocol_request_recv(const struct ct_state *state) {
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
	LOG_INFO("Sending recv request to %d", state->socket_fd);
	if (protocol_write(request, state->socket_fd, "recv", 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write recv request");
		goto out_free;
	}

out_free:
	json_decref(request);
	return rc;
}

int protocol_request_done(const struct ct_state *state, uint32_t archive_id,
			  uint64_t cookie, int status) {
	json_t *request;
	int rc = 0;

	request = json_pack("{ss,si,si,si}",
			    "command", "done",
			    "archive_id", archive_id,
			    "cookie", cookie,
			    "status", status);
	if (!request) {
		rc = -ENOMEM;
		LOG_ERROR(rc, "Could not pack recv request");
		return rc;
	}
	LOG_INFO("Sending done request to %d", state->socket_fd);
	if (protocol_write(request, state->socket_fd, "done", 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write done request");
		goto out_free;
	}

out_free:
	json_decref(request);
	return rc;
}

int protocol_request_queue(const struct ct_state *state,
			   json_t *hai_list) {
	int rc;

	json_t *hal = json_object();
	if (!hal)
		abort();
	if ((rc = protocol_setjson_int(hal, "hal_version", HAL_VERSION)) ||
	    (rc = protocol_setjson_int(hal, "hal_count", json_array_size(hai_list)))) {
		LOG_ERROR(rc, "Could not fill hsm action list");
		json_decref(hal);
		return rc;
	}
	if (state->fsname &&
	    (rc = protocol_setjson_str(hal, "hal_fsname", state->fsname))) {
		LOG_ERROR(rc, "Could not fill hsm action fsname");
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

	LOG_INFO("Sending queue request to %d", state->socket_fd);
	if (protocol_write(request, state->socket_fd, "queue", 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write queue request");
		goto out_free;
	}
out_free:
	json_decref(request);
	return rc;
}


int protocol_request_ehlo(const struct ct_state *state, json_t *hai_list) {
	int rc;

	json_t *request = json_object();
	if (!request)
		abort();

	if ((rc = protocol_setjson_str(request, "command", "ehlo")))
		goto out_free;

	if (state->fsname &&
	    (rc = protocol_setjson_str(request, "fsname", state->fsname)))
		goto out_free;
	if (hai_list &&
	    (rc = protocol_setjson(request, "hai_list", hai_list)))
		goto out_free;

	if (state->config.client_id
	    && (rc = protocol_setjson_str(request, "id", state->config.client_id)))
		goto out_free;

	LOG_INFO("Sending elho request to %d", state->socket_fd);
	if (protocol_write(request, state->socket_fd, "ehlo", 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write queue request");
		goto out_free;
	}
out_free:
	json_decref(request);
	return rc;
}

static int ehlo_cb(void *fd_arg UNUSED, json_t *json, void *arg UNUSED) {
	return protocol_checkerror(json);
}

protocol_read_cb protocol_ehlo_cbs[PROTOCOL_COMMANDS_MAX] = {
	[EHLO] = ehlo_cb,
};
