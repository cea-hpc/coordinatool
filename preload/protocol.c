/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "preload.h"

static int recv_cb(void *fd_arg UNUSED, json_t *json, void *arg) {
	struct hsm_copytool_private *priv = arg;
	int rc;

	rc = protocol_checkerror(json);
	if (rc)
		return rc;

	json_t *hal = json_object_get(json, "hsm_action_list");
	if (!hal) {
		printf("no hal\n");
		return -EINVAL;
	}

	/* XXX find a better place to store it for action_begin */
	unsigned int archive_id =
		protocol_getjson_int(hal, "hal_archive_id", 0);
	priv->state.config.archive_id = archive_id;

	rc = json_hsm_action_list_get(hal, priv->hal,
				      priv->state.config.hsm_action_list_size,
				      NULL, NULL);

	if (rc < 0)
		return rc;
	priv->msgsize = rc;
	return 0;
}

static int done_cb(void *fd_arg UNUSED, json_t *json, void *arg UNUSED) {
	return protocol_checkerror(json);
}

protocol_read_cb copytool_cbs[PROTOCOL_COMMANDS_MAX] = {
	[RECV] = recv_cb,
	[DONE] = done_cb,
};
