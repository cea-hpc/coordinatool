/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>

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


int protocol_archive_ids(int archive_count, int *archives, json_t **out) {
	json_t *archive_id_array;
	int rc, i;

	assert(out);

	if (archive_count == 0) {
		*out = NULL;
		return 0;
	}

	archive_id_array = json_array();
	if (!archive_id_array) {
		rc = -ENOMEM;
		LOG_ERROR(rc, "Could not allocate archive_id list");
		return rc;
	}
	for (i = 0; i < archive_count; i++) {
		rc = json_array_append_new(archive_id_array,
					   json_integer(archives[i]));
		if (rc) {
			rc = -ENOMEM;
			LOG_ERROR(rc, "Could not append to archive_id list");
			return rc;
		}
	}
	*out = archive_id_array;

	return 0;
}

int protocol_request_recv(const struct ct_state *state) {
	json_t *request;
	int rc = 0;


	request = json_pack("{ss,si,si,si,si}",
			    "command", "recv",
			    "max_archive", state->config.max_archive,
			    "max_restore", state->config.max_restore,
			    "max_remove", state->config.max_remove,
			    "max_bytes", state->config.hsm_action_list_size);
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

int protocol_request_done(const struct ct_state *state,
			  uint64_t cookie, struct lu_fid *dfid,
			  int status) {
	json_t *request;
	int rc = 0;

	request = json_pack("{ss,si,so,si}",
			    "command", "done",
			    "hai_cookie", cookie,
			    "hai_dfid", json_fid(dfid),
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

	json_t *request = json_object();
	if (!request)
		abort();

	if (state->fsname &&
	    (rc = protocol_setjson_str(request, "hal_fsname", state->fsname))) {
		LOG_ERROR(rc, "Could not fill hsm action fsname");
		goto out_free;
	}

	if ((rc = protocol_setjson(request, "hsm_action_items", hai_list)) ||
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

	/* use json_object_set for hai and archive_ids to not steal the ref:
	 * we can try to send it many times */
	if (hai_list &&
	    (rc = json_object_set(request, "hai_list", hai_list)))
		goto out_free;

	if (state->archive_ids &&
	    (rc = json_object_set(request, "archive_ids", state->archive_ids)))
		goto out_free;

	if (state->config.client_id &&
	    (rc = protocol_setjson_str(request, "id", state->config.client_id)))
		goto out_free;

	LOG_INFO("Sending elho request to %d", state->socket_fd);
	if (protocol_write(request, state->socket_fd, "ehlo", 0)) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write ehlo request");
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
