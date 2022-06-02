/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <search.h>

#include "coordinatool.h"

void hsm_action_queues_init(struct state *state,
			    struct hsm_action_queues *queues) {
	CDS_INIT_LIST_HEAD(&queues->waiting_restore);
	CDS_INIT_LIST_HEAD(&queues->waiting_archive);
	CDS_INIT_LIST_HEAD(&queues->waiting_remove);
	queues->actions_tree = NULL;
	queues->state = state;
}

static int tree_compare(const void *a, const void *b) {
	__u64 va = *(__u64*)a, vb = *(__u64*)b;

	if (va < vb)
		return -1;
	else if (va == vb)
		return 0;
	return 1;
}

void queue_node_free(struct hsm_action_node *han) {
	redis_delete(han->queues->state, han->info.cookie);
	if (!tdelete(&han->info.cookie, &han->queues->actions_tree,
		     tree_compare))
		abort();
	if (han->hai)
		json_decref(han->hai);
	free(han);
}

struct hsm_action_queues *hsm_action_queues_get(struct state *state,
						unsigned int archive_id,
						unsigned long long flags,
						const char *fsname) {
	if (archive_id == ARCHIVE_ID_UNINIT) {
		LOG_ERROR(-EINVAL, "No archive_id given");
		return NULL;
	}

	if (state->queues.archive_id == ARCHIVE_ID_UNINIT) {
		if (!fsname) {
			LOG_ERROR(-EINVAL, "Tried to get queues for %d before it was set",
				  archive_id);
			return NULL;
		}
		// XXX we only support one archive_id, first one we sees
		// determines what others should be.
		state->queues.archive_id = archive_id;
		state->queues.fsname = xstrdup(fsname);
		state->queues.hal_flags = flags;
	} else {
		if (state->queues.archive_id != archive_id) {
			LOG_ERROR(-EINVAL, "received action list for archive id %d but already seen %d, ignoring these",
				  archive_id, state->queues.archive_id);
			return NULL;
		}
		if (fsname) {
			// XXX strcmp fsname?
			if (state->queues.hal_flags != flags) {
				LOG_ERROR(-EINVAL, "received action list with different flags (got %llx, expected %llx); keeping current flags anyway",
					  flags, state->queues.hal_flags);
				return NULL;
			}
		}
	}

	return &state->queues;
}

struct hsm_action_node *hsm_action_search_queue(struct hsm_action_queues *queues,
						unsigned long cookie) {
	void *key = tfind(&cookie, &queues->actions_tree, tree_compare);

	if (!key)
		return NULL;

	key = *(void **)key;

	struct item_info *item_info =
		caa_container_of(key, struct item_info, cookie);
	return caa_container_of(item_info, struct hsm_action_node, info);
}

/* actually inserts action node to its queue */
int hsm_action_requeue(struct hsm_action_node *han, bool start) {
	struct cds_list_head *head;
	struct hsm_action_queues *queues = han->queues;

	switch (han->info.action) {
	case HSMA_RESTORE:
		head = &queues->waiting_restore;
		queues->state->stats.pending_restore++;
		break;
	case HSMA_ARCHIVE:
		head = &queues->waiting_archive;
		queues->state->stats.pending_archive++;
		break;
	case HSMA_REMOVE:
		head = &queues->waiting_remove;
		queues->state->stats.pending_remove++;
		break;
	default:
		queue_node_free(han);
		return -EINVAL;
	}

	if (start)
		cds_list_add(&han->node, head);
	else
		cds_list_add_tail(&han->node, head);
	return 1;
}

/* move an hsm action node to another queue */
void hsm_action_move(struct hsm_action_queues *queues,
		     struct hsm_action_node *han,
		     bool start) {
	struct cds_list_head *head;

	/* we can't use requeue as we don't want to update stats... */
	switch (han->info.action) {
	case HSMA_RESTORE:
		head = &queues->waiting_restore;
		break;
	case HSMA_ARCHIVE:
		head = &queues->waiting_archive;
		break;
	case HSMA_REMOVE:
		head = &queues->waiting_remove;
		break;
	default:
		/* we got this out of another queue,
		 * this should never happen
		 */
		abort();
	}

	cds_list_del(&han->node);
	han->queues = queues;
	if (start)
		cds_list_add(&han->node, head);
	else
		cds_list_add_tail(&han->node, head);
}

/* checks for duplicate, and if unique enrich and insert node */
int hsm_action_enqueue(struct hsm_action_queues *queues,
		       struct hsm_action_item *hai) {
	struct hsm_action_node *han;
	uint64_t **tree_key;

	if (hai->hai_action == HSMA_CANCEL) {
		// XXX tfind + remove from waiting queue or signal client
		return 0;
	}
	switch (hai->hai_action) {
	case HSMA_RESTORE:
	case HSMA_ARCHIVE:
	case HSMA_REMOVE:
		break;
	default:
		return -EINVAL;
	}

	han = xmalloc(sizeof(struct hsm_action_node));
	han->hai = json_hsm_action_item(hai);
	if (!han->hai) {
		free(han);
		return -ENOMEM;
	}
	han->queues = queues;
	han->info.cookie = hai->hai_cookie;
	han->info.action = hai->hai_action;
	han->info.dfid = hai->hai_dfid;
	han->info.hai_len = hai->hai_len;

	tree_key = tsearch(&han->info.cookie, &queues->actions_tree,
			   tree_compare);
	if (!tree_key)
		abort();
	if (*tree_key != &han->info.cookie) {
		/* duplicate */
		free(han);
		return 0;
	}

	hsm_action_node_enrich(queues->state, han);

	// continue even if this errored out
	(void)redis_insert(queues->state, han);

	return hsm_action_requeue(han, false);
}

/* remove action node from its queue */
void hsm_action_dequeue(struct hsm_action_queues *queues,
			struct hsm_action_node *han) {
	switch (han->info.action) {
	case HSMA_RESTORE:
		queues->state->stats.pending_restore--;
		break;
	case HSMA_ARCHIVE:
		queues->state->stats.pending_archive--;
		break;
	case HSMA_REMOVE:
		queues->state->stats.pending_remove--;
		break;
	default:
		LOG_ERROR(-EINVAL, "requested to dequeue %s",
			  ct_action2str(han->info.action));
	}

	cds_list_del(&han->node);
}
