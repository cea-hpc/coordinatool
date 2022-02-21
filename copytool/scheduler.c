/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/xattr.h>

#include <phobos_store.h>

static bool can_send_to_client(struct state *state, struct client *client,
			       struct hsm_action_item *hai)
{
	char oid[XATTR_SIZE_MAX+1];
	int rc, save_errno, fd;
	ssize_t oidlen;
	char *hostname;

	fd = llapi_open_by_fid(state->mntpath, &hai->hai_fid, O_RDONLY);
	if (fd < 0)
		return false;

	oidlen = fgetxattr(fd, "trusted.hsm_fuid", oid, XATTR_SIZE_MAX);
	save_errno = errno;
	close(fd);
	errno = save_errno;
	if (oidlen < 0) {
		/* ENOATTR seems to be returned if the attribute doesn't exist
		 * but it is not defined on Linux.
		 * ENODATA seems to be equivalent on Linux.
		 */
		return (errno == ENODATA || errno == ENOTSUP);
	}
	oid[oidlen] = '\0';

	rc = phobos_locate(oid, NULL, 0, &hostname);
	if (rc)
		/* not on a phobos backend ? */
		return true;

	return hostname == NULL || !strcmp(hostname, client->id);
}

static int recv_enqueue(struct client *client, json_t *hai_list,
			struct hsm_action_node *han) {
	int max_action;
	int *current_count;

	if (client->max_bytes < sizeof(han->hai) + han->hai.hai_len) {
		return -ERANGE;
	}
	switch (han->hai.hai_action) {
	case HSMA_RESTORE:
		max_action = client->max_restore;
		current_count = &client->current_restore;
		break;
	case HSMA_ARCHIVE:
		max_action = client->max_archive;
		current_count = &client->current_archive;
		break;
	case HSMA_REMOVE:
		max_action = client->max_remove;
		current_count = &client->current_remove;
		break;
	default:
		return -EINVAL;
	}
	if (max_action >= 0 && max_action <= *current_count)
		return -ERANGE;

	json_array_append_new(hai_list, json_hsm_action_item(&han->hai));
	han->client = client;
	(*current_count)++;
	cds_list_add(&han->node, &client->active_requests);

	return 0;
}

void ct_schedule_client(struct state *state, struct client *client) {
	size_t i;

	if (!client->waiting)
		return;

	json_t *hai_list = json_array();
	if (!hai_list)
		abort();

	/* check if there are pending requests
	 * priority restore > remove > archive is hardcoded for now */
	int enqueued_items = 0;
	enum hsm_copytool_action actions[] = {
		HSMA_RESTORE, HSMA_REMOVE, HSMA_ARCHIVE,
	};
	int *max_action[] = { &client->max_restore, &client->max_remove,
		&client->max_archive };
	int *current_count[] = { &client->current_restore,
		&client->current_remove, &client->current_archive };
	unsigned int *running_count[] = { &state->stats.running_restore,
		&state->stats.running_remove, &state->stats.running_archive };
	unsigned int *pending_count[] = { &state->stats.pending_restore,
		&state->stats.pending_remove, &state->stats.pending_archive };

	for (i = 0; i < sizeof(actions) / sizeof(*actions); i++) {
		unsigned int enqueued_pass = 0, pending_pass = *pending_count[i];
		while (client->max_bytes > HAI_SIZE_MARGIN) {
			struct hsm_action_node *han;
			bool requeue;

			if (*max_action[i] >= 0 &&
			    *max_action[i] <= *current_count[i])
				break;

			han = hsm_action_dequeue(&state->queues, actions[i]);
			if (!han)
				break;

			requeue = (actions[i] != HSMA_ARCHIVE)
				&& !can_send_to_client(state, client,
						       &han->hai);
			if (requeue || recv_enqueue(client, hai_list, han)) {
				/* did not fit, requeue - this also makes a new copy */
				hsm_action_requeue(han);
				break;
			}
			LOG_INFO("Sending "DFID" to %s from queues\n",
				 PFID(&han->hai.hai_dfid), client->id);
			enqueued_items++;
			(*running_count[i])++;
			enqueued_pass++;
			/* don't hand in too much work if other clients waiting */
			if (enqueued_pass > pending_pass / state->stats.clients_connected)
				break;
		}
	}

	if (!enqueued_items) {
		json_decref(hai_list);
		return;
	}

	cds_list_del(&client->node_waiting);
	client->waiting = false;

	/* frees hai_list */
	int rc = protocol_reply_recv(client, &state->queues, hai_list, 0, NULL);
	if (rc < 0) {
		LOG_ERROR(rc, "Could not send reply to %s\n", client->id);
		free_client(state, client);
	}
}

void ct_schedule(struct state *state) {
	struct client *client, *c;

	cds_list_for_each_entry_safe(client, c,
								 &state->queues.state->waiting_clients,
								 node_waiting)
		ct_schedule_client(state, client);
}
