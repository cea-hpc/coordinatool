/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>

#include "coordinatool.h"

/* sentinel value for timers we already processed:
 * guaranteed to be less than now and not zero (=unlimited)
 */
#define EXPIRED_DEADLINE 1

/* check batch times */
static bool batch_still_reserved(struct client_batch *batch, uint64_t now_ns)
{
	if (batch->expire_max_ns && batch->expire_max_ns < now_ns)
		return false;
	if (batch->expire_idle_ns && batch->expire_idle_ns < now_ns &&
	    cds_list_empty(&batch->waiting_archive))
		return false;
	return true;
}

/* Find a matching slot.
 * Does not check deadlines. */
static int batch_find_slot(struct client *client, struct hsm_action_node *han)
{
	int i;
	for (i = 0; i < state->config.batch_slots; i++) {
		/* skip empty slots, non-matches */
		if (!client->batch[i].hint) {
			continue;
		}
		if (strcmp(client->batch[i].hint, han->info.data))
			continue;
		return i;
	}
	return -1;
}

/* return list, if han was given this is a new batch so populate hint & start time */
static struct cds_list_head *batch_slot_list(struct client *client,
					     int batch_index,
					     struct hsm_action_node *han,
					     uint64_t now_ns)
{
	struct client_batch *batch = &client->batch[batch_index];

	if (han) {
		LOG_INFO(
			"Batches: client %s (%d): new batch for '%s' (was '%s')",
			client->id, client->fd, han->info.data,
			batch->hint ?: "(free)");
		free(batch->hint);
		batch->hint = xstrdup(han->info.data);
		batch->expire_max_ns =
			state->config.batch_slice_max ?
				now_ns + state->config.batch_slice_max :
				0;
	}
	/* in theory this is redundant because last_ns check always
	 * verifies there is no waiting work as well... */
	batch->expire_idle_ns =
		state->config.batch_slice_idle ?
			now_ns + state->config.batch_slice_idle :
			0;
	return &batch->waiting_archive;
}

struct cds_list_head *schedule_batch_slot_active(struct hsm_action_node *han)
{
	/* only batch archive, with hint data */
	if (han->info.action != HSMA_ARCHIVE || !han->info.data ||
	    !han->info.data[0])
		return NULL;

	/* skip if disabled in config */
	if (state->config.batch_slice_idle == 0)
		return NULL;

	/* If this becomes a performance problem we can make a hash table... */
	struct cds_list_head *clients = &state->stats.clients, *n;
	uint64_t now_ns = gettime_ns();

	/* got a match? */
	cds_list_for_each(n, clients)
	{
		struct client *client =
			caa_container_of(n, struct client, node_clients);
		int i = batch_find_slot(client, han);

		if (i < 0)
			continue;

		/* found a batch, if still reserved use it. */
		if (batch_still_reserved(&client->batch[i], now_ns))
			return batch_slot_list(client, i, NULL, now_ns);

		/* batch is expired... If no other work waiting re-use it anyway for better
		 * locality. */
		if (cds_list_empty(&client->queues.waiting_archive))
			/* pass han to re-set start time */
			return batch_slot_list(client, i, han, now_ns);
	}
	return NULL;
}

struct cds_list_head *schedule_batch_slot_new(struct hsm_action_node *han)
{
	/* only batch archive */
	if (han->info.action != HSMA_ARCHIVE)
		return NULL;

	/* skip if disabled in config */
	if (state->config.batch_slice_idle == 0)
		return NULL;

	assert(han->info.data);

	/* try to get a new slot, we do this in two passes:
	 * - first look for really empty (no hint, or expired & no active work)
	 * - then look for expired to take over, we'll try to re-schedule any
	 * new pending work later
	 */
	struct cds_list_head *clients = &state->stats.clients, *n;
	struct client *client = NULL;
	struct client_batch *batch = NULL;
	int i;
	uint64_t now_ns = gettime_ns();

	/* any empty slot? breadth first search */
	for (i = 0; i < state->config.batch_slots; i++) {
		cds_list_for_each(n, clients)
		{
			client = caa_container_of(n, struct client,
						  node_clients);
			batch = &client->batch[i];

			/* free slot! */
			if (!batch->hint)
				return batch_slot_list(client, i, han, now_ns);

			/* expired and no pending work */
			if (batch_still_reserved(batch, now_ns))
				continue;
			if (!cds_list_empty(&batch->waiting_archive))
				continue;

			return batch_slot_list(client, i, han, now_ns);
		}
	}
	/* Second pass identical, except any expired batch's pending work is
	 * requeued and list is taken over */
	for (i = 0; i < state->config.batch_slots; i++) {
		cds_list_for_each(n, clients)
		{
			client = caa_container_of(n, struct client,
						  node_clients);
			batch = &client->batch[i];
			/* (free slot check skipped as there cannot be any here) */

			if (batch_still_reserved(batch, now_ns))
				continue;

			/* XXX re-insert based on timestamp; get them to reschedule?
			 * probably something like:
			 * - splice this to temporary list head
			 * - reallocate this batch through batch_slot_list for current han
			 * - then we can call hsm_action_requeue() on all items in the temporary
			 *   list
			 * Until then requeue to same client for archive_on_host setting */
			cds_list_splice(&batch->waiting_archive,
					&client->queues.waiting_archive);
			CDS_INIT_LIST_HEAD(&batch->waiting_archive);
			return batch_slot_list(client, i, han, now_ns);
		}
	}

	/* no suitable slot, will be queued to global queue and rescheduled later */
	return NULL;
}

struct cds_list_head *schedule_batch_slot_on_client(struct client *client,
						    struct hsm_action_node *han)
{
	/* check for matches first */
	int i = batch_find_slot(client, han);
	if (i >= 0) {
		uint64_t now_ns = gettime_ns();

		/* if still reserved use just use it,
		 * otherwise refresh start time and use it anyway */
		if (batch_still_reserved(&client->batch[i], now_ns))
			return batch_slot_list(client, i, NULL, now_ns);

		return batch_slot_list(client, i, han, now_ns);
	}

	/* any empty or expired slot? */
	for (i = 0; i < state->config.batch_slots; i++) {
		if (client->batch[i].hint)
			continue;
		return batch_slot_list(client, i, han, gettime_ns());
	}

	return NULL;
}

void batch_reschedule_client(struct client *client)
{
	/* We're only doing this for archives that have multiple queues per client,
	 * and could otherwise get stuck.
	 * Further rework might do something similar for waiting restores */
	struct cds_list_head *schedule_archive_lists[] = {
		&client->queues.waiting_archive, &state->queues.waiting_archive,
		NULL
	};
	struct cds_list_head **archive_lists = schedule_archive_lists;
	struct cds_list_head *n =
		cds_manylists_next(archive_lists[0], &archive_lists);
	/* nothing waiting in local queue, try global one*/
	if (!n)
		return;
	struct hsm_action_node *han =
		caa_container_of(n, struct hsm_action_node, node);

	/* XXX optim: limit to only check every x secs?
	 * we'll need some sort of timer to retrigger this after a while in cas
	 * we missed something. */
	uint64_t now_ns = gettime_ns();

	/* look for empty or expired batches */
	int i;
	for (i = 0; i < state->config.batch_slots; i++) {
		struct client_batch *batch = &client->batch[i];

		if (batch->hint && batch_still_reserved(batch, now_ns))
			continue;
		/* move back only to client queue, not global queue:
		 * - we can't call hsm_action_requeue(_all) here until
		 * after batch_slot_list has been called to overtake the slot
		 * - if we don't schedule and just happend to global queue when
		 * archive_on_host is set, another host will be able to take these
		 * requests and we don't want that
		 * For now just requeue on same host, it might be suboptimal and
		 * lead to batches being split if more same requests come but this
		 * will require some rework to improve
		 */
		cds_list_splice(&batch->waiting_archive,
				&client->queues.waiting_archive);
		CDS_INIT_LIST_HEAD(&batch->waiting_archive);

		struct cds_list_head *list =
			batch_slot_list(client, i, han, now_ns);
		hsm_action_requeue(han, list);

		/* find all other pending actions that could match */
		struct cds_list_head *n, *nnext;
		cds_manylists_for_each_safe(n, nnext, archive_lists)
		{
			han = caa_container_of(n, struct hsm_action_node, node);
			if (strcmp(batch->hint, han->info.data))
				continue;
			hsm_action_requeue(han, list);
		}

		/* get a new han for next slot */
		n = cds_manylists_next(archive_lists[0], &archive_lists);
		if (!n)
			return;
		han = caa_container_of(n, struct hsm_action_node, node);
	}
}

bool batch_slot_can_send(struct client *client, struct hsm_action_node *han)
{
	int i;

	/* only batch archive */
	if (han->info.action != HSMA_ARCHIVE)
		return true;

	/* disabled in config */
	if (state->config.batch_slice_idle == 0)
		return true;

	assert(han->info.data);

	for (i = 0; i < state->config.batch_slots; i++) {
		/* skip empty slots, non-matches */
		if (!client->batch[i].hint)
			continue;
		if (strcmp(client->batch[i].hint, han->info.data))
			continue;
		/* XXX check expire idle/max limits, but if there are other free slots or
		 * no other pending work then re-allocate the slot immediately in place
		 * and allow request.
		 * For now, all requests that had managed to have been queued are allowed */
		client->batch[i].expire_idle_ns =
			state->config.batch_slice_idle ?
				gettime_ns() + state->config.batch_slice_idle :
				0;
		return true;
	}

	hsm_action_requeue(han, NULL);
	return false;
}

static bool client_has_waiting_archives(struct client *client)
{
	/* If archive host mapping is set then we only consider the given
	 * client as requests are routed early, if it is not used then also
	 * look at global queue */
	return !cds_list_empty(&client->queues.waiting_archive) ||
	       (cds_list_empty(&state->config.archive_mappings) &&
		!cds_list_empty(&state->queues.waiting_archive));
}

uint64_t batch_next_expiry(void)
{
	uint64_t closest_ns = INT64_MAX;
	struct cds_list_head *n;
	struct client *client;
	int i;

	cds_list_for_each(n, &state->stats.clients)
	{
		client = caa_container_of(n, struct client, node_clients);

		/* client has no work pending: don't bother */
		if (cds_list_empty(&state->queues.waiting_archive) &&
		    cds_list_empty(&client->queues.waiting_archive))
			continue;

		for (i = 0; i < state->config.batch_slots; i++) {
			if (!client->batch[i].hint)
				continue;
			if (client->batch[i].expire_max_ns > EXPIRED_DEADLINE &&
			    closest_ns > client->batch[i].expire_max_ns)
				closest_ns = client->batch[i].expire_max_ns;
			/* Only consider idle ns if there are waiting requests */
			if (client->batch[i].expire_idle_ns >
				    EXPIRED_DEADLINE &&
			    closest_ns > client->batch[i].expire_idle_ns &&
			    cds_list_empty(&client->batch[i].waiting_archive) &&
			    client_has_waiting_archives(client))
				closest_ns = client->batch[i].expire_idle_ns;
		}
	}

	return closest_ns;
}

void batch_clear_expired(uint64_t now_ns)
{
	struct cds_list_head *n;
	struct client *client;
	int i;

	cds_list_for_each(n, &state->stats.clients)
	{
		client = caa_container_of(n, struct client, node_clients);

		/* client has no work pending: don't bother */
		if (cds_list_empty(&state->queues.waiting_archive) &&
		    cds_list_empty(&client->queues.waiting_archive))
			continue;

		for (i = 0; i < state->config.batch_slots; i++) {
			if (!client->batch[i].hint)
				continue;
			if (client->batch[i].expire_max_ns &&
			    now_ns > client->batch[i].expire_max_ns)
				client->batch[i].expire_max_ns =
					EXPIRED_DEADLINE;
			if (client->batch[i].expire_idle_ns &&
			    now_ns > client->batch[i].expire_idle_ns &&
			    cds_list_empty(&client->batch[i].waiting_archive) &&
			    client_has_waiting_archives(client))
				client->batch[i].expire_idle_ns =
					EXPIRED_DEADLINE;
		}
	}
}
