/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "client.h"

static int status_cb(void *fd_arg UNUSED, json_t *json, void *arg UNUSED) {
	printf("Got status reply:\n");
	protocol_write(json, STDOUT_FILENO, JSON_INDENT(2));
	printf("\n");
	return 0;
}

static int recv_cb(void *fd_arg UNUSED, json_t *json, void *arg) {
	struct ct_state *state = arg;
	printf("Got recv reply:\n");
	protocol_write(json, STDOUT_FILENO, JSON_INDENT(2));
	printf("\n");

	json_t *hal = json_object_get(json, "hsm_action_list");
	if (!hal) {
		printf("no hal\n");
		return -EINVAL;
	}
	unsigned int archive_id =
		protocol_getjson_int(hal, "hal_archive_id", 0);
	json_t *hai_list = json_object_get(hal, "list");
	if (!hai_list) {
		printf("no hal->list\n");
		return -EINVAL;
	}
	size_t i;
	json_t *hai;
	int rc;
	json_array_foreach(hai_list, i, hai) {
		uint64_t cookie = protocol_getjson_int(hai, "hai_cookie", 0);
		if (!cookie) {
			printf("cookie not set?\n");
			return -EINVAL;
		}
		rc = protocol_request_done(state, archive_id, cookie, 0);
		if (rc)
			return rc;
	}

	return 0;
}

static int done_cb(void *fd_arg UNUSED, json_t *json, void *arg UNUSED) {
	printf("Got done reply:\n");
	protocol_write(json, STDOUT_FILENO, JSON_INDENT(2));
	printf("\n");
	return 0;
}

static int queue_cb(void *fd_arg UNUSED, json_t *json, void *arg UNUSED) {
	printf("Got queue reply:\n");
	protocol_write(json, STDOUT_FILENO, JSON_INDENT(2));
	printf("\n");
	return 0;
}

protocol_read_cb protocol_cbs[PROTOCOL_COMMANDS_MAX] = {
	[STATUS] = status_cb,
	[RECV] = recv_cb,
	[DONE] = done_cb,
	[QUEUE] = queue_cb,
};
