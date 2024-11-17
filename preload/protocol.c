/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "preload.h"

static int action_list_get_cb(struct hsm_action_list *hal UNUSED,
			      struct hsm_action_item *hai, json_t *hai_json,
			      void *arg)
{
	struct hsm_copytool_private *priv = arg;
	struct action_tree_node *node = xmalloc(sizeof(*node));

	node->key.cookie = hai->hai_cookie;
	node->key.dfid = hai->hai_dfid;
	node->hai = json_incref(hai_json);

	action_insert(priv, node);
	return 0;
}

static int recv_cb(void *fd_arg UNUSED, json_t *json, void *arg)
{
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

	rc = json_hsm_action_list_get(hal, priv->hal,
				      priv->state.config.hsm_action_list_size,
				      action_list_get_cb, priv);

	if (rc < 0)
		return rc;
	priv->msgsize = rc;
	return 0;
}

static int done_cb(void *fd_arg UNUSED, json_t *json, void *arg UNUSED)
{
	return protocol_checkerror(json);
}

protocol_read_cb copytool_cbs[PROTOCOL_COMMANDS_MAX] = {
	[RECV] = recv_cb,
	[DONE] = done_cb,
};
