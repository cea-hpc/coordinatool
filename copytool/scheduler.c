/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"

/* schedule decision helper */

/* fill in static action item informations */
void hsm_action_node_enrich(struct state *state UNUSED,
			    struct hsm_action_node *han UNUSED) {
	// XXX actually fill in han->info with static infos
	// At this point we could also want to pre-assign the node to a client,
	// this can be done by setting han->queues = &client->queues
}

/* check if we still want to schedule action to client.
 * We can put node back in global queue or just skip it
 * returns true if can schedule
 * if needed later we can make this return a decision enum
 * OK/NEXT_ACTION/NEXT_CLIENT
 */
static bool schedule_can_send(struct client *client UNUSED,
			      struct hsm_action_node *han UNUSED) {
	// XXX check things
	// There might be a way to signal we just assigned node and
	// phobos_locate doesn't need rechecking? but in general this can
	// be called much later so we should recheck.
	// to requeue in another queue, update han->queues
	// (we have global queue in han->queues->state->queues)
	return true;
}

/* enqueue to json list */
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

void ct_schedule_client(struct state *state,
			struct client *client) {
	if (client->state != CLIENT_WAITING)
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
	for (size_t i = 0; i < sizeof(actions) / sizeof(*actions); i++) {
		unsigned int enqueued_pass = 0, pending_pass = *pending_count[i];
		struct hsm_action_queues *queues = &client->queues;
		while (client->max_bytes > HAI_SIZE_MARGIN) {
			if (*max_action[i] >= 0 &&
			    *max_action[i] <= *current_count[i]) {
				break;
			}

			struct hsm_action_node *han;
			han = hsm_action_dequeue(queues, actions[i]);
			if (!han && queues == &client->queues) {
				queues = &state->queues;
				continue;
			}
			if (!han)
				break;
			if (!schedule_can_send(client, han)) {
				hsm_action_requeue(han);
				continue;
			}
			if (recv_enqueue(client, hai_list, han)) {
				/* did not fit, requeue - this also makes a new copy */
				hsm_action_requeue(han);
				break;
			}
			LOG_INFO("Sending "DFID" to %d from queues" ,
				 PFID(&han->hai.hai_dfid), client->fd);
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
	client->state = CLIENT_CONNECTED;

	// frees hai_list
	int rc = protocol_reply_recv(client, &state->queues, hai_list, 0, NULL);
	if (rc < 0) {
		LOG_ERROR(rc, "Could not send reply to %s", client->id);
		free_client(state, client);
	}
}

void ct_schedule(struct state *state) {
	struct cds_list_head *n, *nnext;

	cds_list_for_each_safe(n, nnext, &state->waiting_clients) {
		struct client *client = caa_container_of(n, struct client,
				node_waiting);
		ct_schedule_client(state, client);
	}
}
