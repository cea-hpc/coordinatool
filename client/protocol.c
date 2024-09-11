/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "client.h"

static int status_cb(void *fd_arg UNUSED, json_t *json, void *arg UNUSED) {
	printf("Got status reply:\n");
	protocol_write(json, STDOUT_FILENO, "stdout", JSON_INDENT(2));
	printf("\n");
	return 0;
}

static int recv_cb(void *fd_arg UNUSED, json_t *json, void *arg) {
	struct client *client = arg;
	printf("Got recv reply:\n");
	protocol_write(json, STDOUT_FILENO, "stdout", JSON_INDENT(2));
	printf("\n");

	json_t *hal = json_object_get(json, "hsm_action_list");
	if (!hal) {
		printf("no hal\n");
		return -EINVAL;
	}
	json_t *hai_list = json_object_get(hal, "list");
	if (!hai_list) {
		printf("no hal->list\n");
		return -EINVAL;
	}
	size_t i;
	json_t *hai;
	int rc;
	json_array_foreach(hai_list, i, hai) {
		uint64_t cookie;
		struct lu_fid dfid;
		if (json_hsm_action_key_get(hai, &cookie, &dfid)) {
			printf("cookie/dfid not set - version mismatch?\n");
			return -EINVAL;
		}
		// If we send done here, the coordinatool will consider the archive done and
		// tell lustre, clearing the hsm request.
		if (client->mode == MODE_DRAIN) {
			rc = protocol_request_done(&client->state, cookie, &dfid, 0);
			if (rc)
				return rc;
		}
	}

	// get work again
	return protocol_request_recv(&client->state);
}

static int done_cb(void *fd_arg UNUSED, json_t *json, void *arg UNUSED) {
	printf("Got done reply:\n");
	protocol_write(json, STDOUT_FILENO, "stdout", JSON_INDENT(2));
	printf("\n");
	return 0;
}

static int queue_cb(void *fd_arg UNUSED, json_t *json, void *arg) {
	struct client *client = arg;

	printf("Got queue reply:\n");
	protocol_write(json, STDOUT_FILENO, "stdout", JSON_INDENT(2));
	printf("\n");

	int status = protocol_getjson_int(json, "status", 0);
	if (status)
		return -status;

	int enqueued = protocol_getjson_int(json, "enqueued", 0);
	int skipped = protocol_getjson_int(json, "skipped", 0);
	if (client->sent_items != enqueued + skipped) {
		printf("didn't process all records (expected %d, got %d+%d)\n",
			client->sent_items, enqueued, skipped);
		return -EINVAL;
	}

	return 0;
}

protocol_read_cb protocol_cbs[PROTOCOL_COMMANDS_MAX] = {
	[STATUS] = status_cb,
	[RECV] = recv_cb,
	[DONE] = done_cb,
	[QUEUE] = queue_cb,
};
