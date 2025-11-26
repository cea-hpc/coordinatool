/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>

#include "coordinatool.h"

/* schedule decision helper.
 * sub-helpers return > 0 if scheduled, 0 if skipped */
struct client *find_client(struct cds_list_head *clients_list,
			   const char *hostname)
{
	struct cds_list_head *n;

	cds_list_for_each(n, clients_list)
	{
		struct client *client =
			caa_container_of(n, struct client, node_clients);

		if (!strcmp(hostname, client->id)) {
			return client;
		}
	}
	return NULL;
}

struct cds_list_head *schedule_on_client(struct client *client,
					 struct hsm_action_node *han)
{
	report_action(han, "assign " DFID " %s\n", PFID(&han->info.dfid),
		      client->id);
	/* for archive, respect slots if used. Otherwise just get queue */
	if (han->info.action == HSMA_ARCHIVE) {
		struct cds_list_head *list =
			schedule_batch_slot_on_client(client, han);
		if (list)
			return list;
	}
	return get_queue_list(&client->queues, han);
}

static struct cds_list_head *schedule_host_mapping(struct hsm_action_node *han)
{
	/* only doing this for archive for now */
	if (han->info.action != HSMA_ARCHIVE)
		return NULL;

	struct cds_list_head *n;
	struct host_mapping *mapping;
	bool found = false;

	cds_list_for_each(n, &state->config.archive_mappings)
	{
		mapping = caa_container_of(n, struct host_mapping, node);
		if (strstr(han->info.data, mapping->tag)) {
			found = true;
			break;
		}
	}

	if (!found)
		return NULL;

	int first_idx = rand() % mapping->count;
	int idx = first_idx;
	const char *hostname = mapping->hosts[idx];
	struct cds_list_head *clients = &state->stats.connected_clients;
	struct client *client;
	/* try all configured hosts until one found online,
	 * and if none all hosts again with disconnected clients,
	 * and if none of that either we'll create a dummy disconnected
	 * client for this request: if host settings are set this should
	 * never go to a global queue. */
	while ((client = find_client(clients, hostname)) == NULL) {
		idx = (idx + 1) % mapping->count;
		if (idx == first_idx) {
			if (clients == &state->stats.disconnected_clients)
				break;
			clients = &state->stats.disconnected_clients;
		}
		hostname = mapping->hosts[idx];
	}
	if (!client) {
		/* note: it's a disconnected client with expiry, but if it expires
		 * without any client connecting then requests here will be rescheduled
		 * through this function again, and that'll recreate a new client.. */
		client = client_new_disconnected(mapping->hosts[idx]);
	}

	return schedule_on_client(client, han);
}

/* fill in static action item informations */
struct cds_list_head *hsm_action_node_schedule(struct hsm_action_node *han)
{
	struct cds_list_head *list;

	/* If host mapping is active with multiple candidates, we'd need to
	 * look for slots in all matches first so just look for currently
	 * running slots first. If host mapping is removed the two schedule
	 * batch slot functions can likely be merged together */
	list = schedule_batch_slot_active(han);
	if (list)
		return list;

	list = schedule_host_mapping(han);
	if (list)
		return list;

	list = schedule_batch_slot_new(han);
	if (list)
		return list;

#if HAVE_PHOBOS
	return phobos_schedule(han);
#endif
}

/* check if we still want to schedule action to client.
 * We can put node back in global queue or just skip it
 * returns true if can schedule
 * if needed later we can make this return a decision enum
 * OK/NEXT_ACTION/NEXT_CLIENT
 */
static bool schedule_can_send(struct client *client,
			      struct hsm_action_node *han)
{
	if (!batch_slot_can_send(client, han))
		return false;
#if HAVE_PHOBOS
	return phobos_can_send(client, han);
#else
	return true;
#endif
}

/* enqueue to json list */
static int recv_enqueue(struct client *client, json_t *hai_list,
			struct hsm_action_node *han, size_t *enqueued_bytes)
{
	int max_action;
	int *current_count;

	if ((*enqueued_bytes) + sizeof(struct hsm_action_item) +
		    han->info.hai_len >
	    client->max_bytes) {
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
	(*enqueued_bytes) += sizeof(struct hsm_action_item) + han->info.hai_len;

	return 0;
}

/* accept the archive_id if it is in the list, but also if there
 * is no list or if it is empty (no archive_id set by client) */
static bool accept_archive_id(int *archives, int archive_id)
{
	bool rc = true;

	if (!archives)
		return true;

	while (*archives) {
		if (*archives == archive_id)
			return true;
		archives++;
		rc = false;
	}

	return rc;
}

void ct_schedule_client(struct client *client)
{
	if (client->status != CLIENT_WAITING)
		return;

	json_t *hai_list = json_array();
	if (!hai_list)
		abort();

	/* check if there are pending requests
	 * priority restore > remove > archive is hardcoded for now */
	size_t enqueued_bytes = 0;
	struct cds_list_head *schedule_restore_lists[] = {
		&client->queues.waiting_restore, &state->queues.waiting_restore,
		NULL
	};
	struct cds_list_head *schedule_remove_lists[] = {
		&client->queues.waiting_remove, &state->queues.waiting_remove,
		NULL
	};
	/* for archives we either only use the "global" lists or the batch lists,
	 * this is a config-time switch. */
	struct cds_list_head *schedule_archive_lists[] = {
		&client->queues.waiting_archive, &state->queues.waiting_archive,
		NULL
	};
	/* VLAs are bad, but batch_slots is supposed to be tiny.. */
	struct cds_list_head
		*schedule_archive_batch_lists[state->config.batch_slots + 1];
	int i;
	for (i = 0; i < state->config.batch_slots; i++) {
		schedule_archive_batch_lists[i] =
			&client->batch[i].waiting_archive;
	}
	schedule_archive_batch_lists[state->config.batch_slots] = NULL;

	/* move anything we can from client archive queue to each batch */
	batch_reschedule_client(client);

	struct cds_list_head **schedule_lists[] = {
		schedule_restore_lists,
		schedule_remove_lists,
		state->config.batch_slots ? schedule_archive_batch_lists :
					    schedule_archive_lists,
	};
	int *max_action[] = { &client->max_restore, &client->max_remove,
			      &client->max_archive };
	int *current_count[] = { &client->current_restore,
				 &client->current_remove,
				 &client->current_archive };
	unsigned int *pending_count[] = { &state->stats.pending_restore,
					  &state->stats.pending_remove,
					  &state->stats.pending_archive };
	uint32_t archive_id;
	uint64_t hal_flags;
	for (size_t i = 0; i < countof(max_action); i++) {
		unsigned int enqueued_pass = 0,
			     pending_pass = *pending_count[i];
		struct cds_list_head *n, *nnext;
		int j, stuck = 0;
		cds_manylists_for_each_safe(j, n, nnext, schedule_lists[i])
		{
			int *extra_count = NULL;
			int extra_max = 0;
			if (stuck++ > 100) {
				/* this is a poor workaround until a better solution
				 * is ready: at least the can send callback can re-enqueue
				 * in the same queue we're pulling from, making this loop never
				 * end. Just stop after 100 items, we can always send more work
				 * in next iteration if it was actually progressing.
				 */
				break;
			}
			if (client->max_archive >= 0 &&
			    state->config.batch_slots &&
			    max_action[i] == &client->max_archive) {
				/* this is a batch */
				assert(j < state->config.batch_slots);
				extra_count = &client->batch[j].current_count;
				/* round up... */
				extra_max = (*max_action[i] +
					     state->config.batch_slots - 1) /
					    state->config.batch_slots;
			}
			if (enqueued_bytes >
			    client->max_bytes - HAI_SIZE_MARGIN) {
				break;
			}
			if (*max_action[i] >= 0 &&
			    *max_action[i] <= *current_count[i]) {
				break;
			}
			if (extra_max >= 0 && extra_count &&
			    *extra_count >= extra_max) {
				break;
			}

			struct hsm_action_node *han = caa_container_of(
				n, struct hsm_action_node, node);
			if (enqueued_bytes == 0) {
				if (!accept_archive_id(client->archives,
						       han->info.archive_id)) {
					continue;
				}
				archive_id = han->info.archive_id;
				hal_flags = han->info.hal_flags;
			}
			if ((archive_id != han->info.archive_id ||
			     hal_flags != han->info.hal_flags)) {
				/* can only send one archive id at a time */
				continue;
			}
			if (!schedule_can_send(client, han)) {
				continue;
			}
			if (recv_enqueue(client, hai_list, han,
					 &enqueued_bytes)) {
				break;
			}
			report_action(han, "sent " DFID " %s\n",
				      PFID(&han->info.dfid), client->id);
			LOG_INFO("%s (%d): Sending " DFID " (cookie %lx)",
				 client->id, client->fd, PFID(&han->info.dfid),
				 han->info.cookie);
			han->current_count = extra_count;
			hsm_action_start(han, client);
			enqueued_pass++;
			/* don't hand in too much work if other clients waiting */
			if (enqueued_pass >
			    pending_pass / state->stats.clients_connected)
				break;
		}
	}

	if (!enqueued_bytes) {
		json_decref(hai_list);
		return;
	}

	cds_list_del(&client->waiting_node);
	client->status = CLIENT_READY;

	// frees hai_list
	int rc = protocol_reply_recv(client, state->fsname, archive_id,
				     hal_flags, hai_list, 0, NULL);
	if (rc < 0) {
		LOG_ERROR(rc, "%s (%d): Could not send reply", client->id,
			  client->fd);
		client_disconnect(client);
	}
}

void ct_schedule(bool rearm_timers)
{
	struct cds_list_head *n, *nnext;

	cds_list_for_each_safe(n, nnext, &state->waiting_clients)
	{
		struct client *client =
			caa_container_of(n, struct client, waiting_node);
		ct_schedule_client(client);
	}

	if (rearm_timers)
		timer_rearm();
}
