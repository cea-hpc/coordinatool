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
	// to requeue in another queue, use hsm_action_move(newqueue, han, start)
	// (we have global queue in han->queues->state->queues)
	// XXX2, also probably want to signal if we want to requeue
	// at start or not?
	// just adding a ts and letting it be automatic in this case would
	// probably be ok: in most case we're inserting at start so insertion
	// would be O(1). For very long lists it might get slow though.
	return true;
}

/* enqueue to json list */
static int recv_enqueue(struct client *client, json_t *hai_list,
			struct hsm_action_node *han, size_t *enqueued_bytes) {
	int max_action;
	int *current_count;

	if ((*enqueued_bytes) + sizeof(struct hsm_action_item) + han->info.hai_len
			> client->max_bytes) {
		return -ERANGE;
	}
	switch (han->info.action) {
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

	json_array_append(hai_list, han->hai);
	han->client = client;
	(*current_count)++;
	(*enqueued_bytes) += sizeof(struct hsm_action_item) + han->info.hai_len;

	return 0;
}

/* scheduling would normally use cds_list_for_each_safe here but
 * we want to chain both queues, so cheat a bit:
 * this is cds_list_for_each_safe with starting condition from
 * first list, end condition with second list head and
 * a bridge from first to second list when first list ends */
#define cds_twolists_next(p, head1, head2) \
	((p)->next == (head1) ? (head2)->next : (p)->next)
#define cds_twolists_for_each_safe(pos, p, head1, head2) \
	for (pos = cds_twolists_next(head1, head1, head2), \
			p = cds_twolists_next(pos, head1, head2); \
		pos != (head2); \
		pos = p, p = cds_twolists_next(p, head1, head2))

void ct_schedule_client(struct state *state,
			struct client *client) {
	if (client->state != CLIENT_WAITING)
		return;

	json_t *hai_list = json_array();
	if (!hai_list)
		abort();

	/* check if there are pending requests
	 * priority restore > remove > archive is hardcoded for now */
	size_t enqueued_bytes = 0;
	struct cds_list_head *client_waiting_lists[] = {
		&client->queues.waiting_restore,
		&client->queues.waiting_remove,
		&client->queues.waiting_archive,
	};
	struct cds_list_head *state_waiting_lists[] = {
		&state->queues.waiting_restore,
		&state->queues.waiting_remove,
		&state->queues.waiting_archive,
	};
	int *max_action[] = { &client->max_restore, &client->max_remove,
		&client->max_archive };
	int *current_count[] = { &client->current_restore,
		&client->current_remove, &client->current_archive };
	unsigned int *running_count[] = { &state->stats.running_restore,
		&state->stats.running_remove, &state->stats.running_archive };
	unsigned int *pending_count[] = { &state->stats.pending_restore,
		&state->stats.pending_remove, &state->stats.pending_archive };
	/* archive_id cannot be 0, use it as check */
	uint32_t archive_id = 0;
	uint64_t hal_flags;
	for (size_t i = 0; i < sizeof(max_action) / sizeof(*max_action); i++) {
		unsigned int enqueued_pass = 0, pending_pass = *pending_count[i];
		struct hsm_action_queues *queues = &client->queues;
		struct cds_list_head *n, *nnext;
		cds_twolists_for_each_safe(n, nnext, client_waiting_lists[i],
					   state_waiting_lists[i]) {
			if (enqueued_bytes > client->max_bytes - HAI_SIZE_MARGIN) {
				break;
			}
			if (*max_action[i] >= 0 &&
			    *max_action[i] <= *current_count[i]) {
				break;
			}

			struct hsm_action_node *han =
				caa_container_of(n, struct hsm_action_node, node);
			if (archive_id != 0 &&
			    (archive_id != han->info.archive_id ||
			     hal_flags != han->info.hal_flags)) {
				/* can only lsend one archive id at a time */
				continue;
			}
			if (!schedule_can_send(client, han)) {
				continue;
			}
			if (recv_enqueue(client, hai_list, han,
					 &enqueued_bytes)) {
				break;
			}
			LOG_INFO("Sending "DFID" to %d from queues" ,
				 PFID(&han->info.dfid), client->fd);
			if (archive_id == 0) {
				archive_id = han->info.archive_id;
				hal_flags = han->info.hal_flags;
			}
			redis_assign_request(state, client, han);
			hsm_action_dequeue(queues, han);
			cds_list_add(&han->node, &client->active_requests);
			(*running_count[i])++;
			enqueued_pass++;
			/* don't hand in too much work if other clients waiting */
			if (enqueued_pass > pending_pass / state->stats.clients_connected)
				break;
		}
	}

	if (!archive_id) {
		json_decref(hai_list);
		return;
	}

	cds_list_del(&client->node_waiting);
	client->state = CLIENT_CONNECTED;

	// frees hai_list
	int rc = protocol_reply_recv(client, state->fsname, archive_id,
				     hal_flags, hai_list, 0, NULL);
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
