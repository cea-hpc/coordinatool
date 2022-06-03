/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>
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
	// XXX check action really isn't in a queue?
#ifdef DEBUG_ACTION_NODE
	assert(han->magic == DEBUG_ACTION_NODE);
#endif
	LOG_DEBUG("freeing han for " DFID " node %p", PFID(&han->info.dfid),
		  (void*)&han->node);
	redis_delete(han->queues->state, han->info.cookie);
	if (!tdelete(&han->info.cookie, &han->queues->actions_tree,
		     tree_compare))
		abort();
	if (han->hai)
		json_decref(han->hai);
	free(han);
}

struct hsm_action_node *hsm_action_search_queue(struct hsm_action_queues *queues,
						unsigned long cookie) {
	void *key = tfind(&cookie, &queues->actions_tree, tree_compare);

	if (!key)
		return NULL;

	key = *(void **)key;

	struct item_info *item_info =
		caa_container_of(key, struct item_info, cookie);
	struct hsm_action_node *han =
		caa_container_of(item_info, struct hsm_action_node, info);
#ifdef DEBUG_ACTION_NODE
	assert(han->magic == DEBUG_ACTION_NODE);
#endif
	return han;
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

	LOG_DEBUG("Inserting han for " DFID " node %p at %s of %p",
		  PFID(&han->info.dfid), (void*)&han->node,
		  start ? "start" : "tail", (void*)head);
#ifdef DEBUG_ACTION_NODE
	assert(han->magic == DEBUG_ACTION_NODE);
#endif
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

#ifdef DEBUG_ACTION_NODE
	assert(han->magic == DEBUG_ACTION_NODE);
#endif
	cds_list_del(&han->node);
	han->queues = queues;
	if (start)
		cds_list_add(&han->node, head);
	else
		cds_list_add_tail(&han->node, head);
}

static int hsm_action_enqueue_common(struct state *state,
				     struct hsm_action_node *han) {
	uint64_t **tree_key;

	tree_key = tsearch(&han->info.cookie, &han->queues->actions_tree,
			   tree_compare);
	if (!tree_key)
		abort();
	if (*tree_key != &han->info.cookie) {
		/* duplicate */
		json_decref(han->hai);
		free(han);
		return 0;
	}

	hsm_action_node_enrich(state, han);

	// continue even if this errored out
	(void)redis_insert(state, han);

	return hsm_action_requeue(han, false);
}

int hsm_action_enqueue_json(struct state *state, json_t *json_hai,
			    int64_t timestamp) {
	struct hsm_action_node *han;
	struct hsm_action_item hai;
	int rc;

	rc = json_hsm_action_item_get(json_hai, &hai, sizeof(hai));
	// overflow is ok: we don't care about data
	if (rc && rc != -EOVERFLOW) {
		LOG_WARN(rc, "Got invalid hai from client: skipping");
		return rc;
	}

	han = xmalloc(sizeof(struct hsm_action_node));
#ifdef DEBUG_ACTION_NODE
	han->magic = DEBUG_ACTION_NODE;
#endif
	han->queues = &state->queues;
	han->info.cookie = hai.hai_cookie;
	han->info.action = hai.hai_action;
	han->info.dfid = hai.hai_dfid;
	han->info.hai_len = hai.hai_len;
	han->info.archive_id = protocol_getjson_int(json_hai, "hal_archive_id", 0);
	han->info.hal_flags = protocol_getjson_int(json_hai, "hal_flags", 0);
	if (!han->info.archive_id) {
		LOG_WARN(rc, "hai from client did not contain archive_id");
		free(han);
		return -EINVAL;
	}
	switch (hai.hai_action) {
	case HSMA_RESTORE:
	case HSMA_ARCHIVE:
	case HSMA_REMOVE:
		break;
	default:
		LOG_WARN(rc, "hai from client had invalid action %d\n", hai.hai_action);
		free(han);
		return -EINVAL;
	}

	han->info.timestamp = protocol_getjson_int(json_hai, "timestamp", 0);
	if (!han->info.timestamp) {
		han->info.timestamp = timestamp;
		(void)protocol_setjson_int(json_hai, "timestamp", timestamp);
	}

	han->hai = json_incref(json_hai);

	return hsm_action_enqueue_common(state, han);
}

/* checks for duplicate, and if unique enrich and insert node */
int hsm_action_enqueue(struct state *state,
		       struct hsm_action_item *hai,
		       uint32_t archive_id, uint64_t hal_flags,
		       int64_t timestamp) {
	struct hsm_action_node *han;

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
#ifdef DEBUG_ACTION_NODE
	han->magic = DEBUG_ACTION_NODE;
#endif
	han->hai = json_hsm_action_item(hai, archive_id, hal_flags);
	if (!han->hai) {
		free(han);
		return -ENOMEM;
	}
	han->queues = &state->queues;
	han->info.cookie = hai->hai_cookie;
	han->info.action = hai->hai_action;
	han->info.dfid = hai->hai_dfid;
	han->info.hai_len = hai->hai_len;
	han->info.archive_id = archive_id;
	han->info.hal_flags = hal_flags;
	han->info.timestamp = timestamp;
	// ignore failure here: the worst that can happen is we'll get
	// order wrong on recovery
	(void)protocol_setjson_int(han->hai, "timestamp", timestamp);

	return hsm_action_enqueue_common(state, han);
}

/* remove action node from its queue */
void hsm_action_dequeue(struct hsm_action_queues *queues,
			struct hsm_action_node *han) {
#ifdef DEBUG_ACTION_NODE
	assert(han->magic == DEBUG_ACTION_NODE);
#endif
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
