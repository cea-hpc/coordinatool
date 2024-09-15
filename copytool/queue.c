/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>
#include <search.h>

#include "config.h"
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
	const struct item_info *va = a, *vb = b;

	/* lustre only guarantees identity per mdt.
	 * We don't have mdt index in action info, so use cookie and fid
	 * (there can be multiple requests on a single fid as well, but these
	 * must be on the same mdt so cookie will be different)
	 */

	if (va->cookie < vb->cookie)
		return -1;
	if (va->cookie > vb->cookie)
		return 1;
	return memcmp(&va->dfid, &vb->dfid, sizeof(va->dfid));
}

static void _queue_node_free(struct hsm_action_node *han, bool final_cleanup) {
#ifdef DEBUG_ACTION_NODE
	assert(han->magic == DEBUG_ACTION_NODE);
#endif
	LOG_DEBUG("freeing han for " DFID " node %p", PFID(&han->info.dfid),
		  (void*)&han->node);
	if (!final_cleanup) {
		redis_delete_request(han->queues->state, han->info.cookie,
				     &han->info.dfid);
		cds_list_del(&han->node);
#if HAVE_PHOBOS
		free(han->info.hsm_fuid);
#endif
		if (!tdelete(&han->info,
			     &han->queues->state->queues.actions_tree,
			     tree_compare))
			abort();
	}
	free((void*)han->info.data);
	if (han->hai)
		json_decref(han->hai);
	free(han);
}

void queue_node_free(struct hsm_action_node *han) {
	_queue_node_free(han, false);
}

static void tree_free_cb(void *nodep) {
	struct item_info *item_info =
		caa_container_of(nodep, struct item_info, cookie);
	struct hsm_action_node *han =
		caa_container_of(item_info, struct hsm_action_node, info);

	_queue_node_free(han, true);
}


void hsm_action_free_all(struct state *state) {
	tdestroy(state->queues.actions_tree, tree_free_cb);
}

struct hsm_action_node *hsm_action_search_queue(struct hsm_action_queues *queues,
						unsigned long cookie,
						struct lu_fid *dfid) {
	struct item_info search_item = {
		.cookie = cookie,
		.dfid = *dfid,
	};
	void *key = tfind(&search_item, &queues->actions_tree, tree_compare);

	if (!key)
		return NULL;

	struct item_info *item_info = *(void **)key;

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
	bool was_running = (han->client != NULL);

	// in theory we're only queueing at start if it was already running
	assert(was_running == start);
	han->client = NULL;
	if (was_running)
		redis_deassign_request(queues->state, han);

	switch (han->info.action) {
	case HSMA_RESTORE:
		head = &queues->waiting_restore;
		queues->state->stats.pending_restore++;
		if (was_running)
			queues->state->stats.running_restore--;
		break;
	case HSMA_ARCHIVE:
		head = &queues->waiting_archive;
		queues->state->stats.pending_archive++;
		if (was_running)
			queues->state->stats.running_archive--;
		break;
	case HSMA_REMOVE:
		head = &queues->waiting_remove;
		queues->state->stats.pending_remove++;
		if (was_running)
			queues->state->stats.running_remove--;
		break;
	default:
		// free expects a list in good shape
		CDS_INIT_LIST_HEAD(&han->node);
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
	struct item_info **tree_key;

	tree_key = tsearch(&han->info, &han->queues->actions_tree,
			   tree_compare);
	if (!tree_key)
		abort();
	if (*tree_key != &han->info) {
		/* duplicate */
		json_decref(han->hai);
		free(han);
		return -EEXIST;
	}

	hsm_action_node_enrich(state, han);

	redis_store_request(state, han);

	return hsm_action_requeue(han, false);
}

int hsm_action_enqueue_json(struct state *state, json_t *json_hai,
			    int64_t timestamp,
			    struct hsm_action_node **han_out,
			    const char *requestor) {
	struct hsm_action_node *han;
	struct hsm_action_item hai;
	const char *data;
	int rc;

	rc = json_hsm_action_item_get(json_hai, &hai, sizeof(hai), &data);
	// overflow is ok: we don't care about hai_data as we reuse the json
	if (rc && rc != -EOVERFLOW) {
		LOG_WARN(rc, "%s: Could not process invalid hai: skipping", requestor);
		return rc;
	}

	han = xcalloc(1, sizeof(struct hsm_action_node));
#ifdef DEBUG_ACTION_NODE
	han->magic = DEBUG_ACTION_NODE;
	CDS_INIT_LIST_HEAD(&han->node);
#endif
	han->queues = &state->queues;
	han->info.cookie = hai.hai_cookie;
	han->info.action = hai.hai_action;
	han->info.dfid = hai.hai_dfid;
	han->info.hai_len = hai.hai_len;
	han->info.data = xstrdup(data);
	han->info.archive_id = protocol_getjson_int(json_hai, "hal_archive_id", 0);
	han->info.hal_flags = protocol_getjson_int(json_hai, "hal_flags", 0);
	if (!han->info.archive_id) {
		LOG_WARN(-EINVAL, "%s: hai did not contain archive_id", requestor);
		free(han);
		return -EINVAL;
	}
	switch (hai.hai_action) {
	case HSMA_RESTORE:
	case HSMA_ARCHIVE:
	case HSMA_REMOVE:
		break;
	default:
		LOG_WARN(rc, "%s: hai had invalid action %d\n",
			 requestor, hai.hai_action);
		free(han);
		return -EINVAL;
	}

	han->info.timestamp = protocol_getjson_int(json_hai, "timestamp", 0);
	if (!han->info.timestamp) {
		han->info.timestamp = timestamp;
		(void)protocol_setjson_int(json_hai, "timestamp", timestamp);
	}

	han->hai = json_incref(json_hai);

	rc = hsm_action_enqueue_common(state, han);
	if (rc < 0 && rc != -EEXIST) {
		return rc;
	}

	if (han_out) {
		*han_out = (rc == -EEXIST ? NULL : han);
	}

	return rc < 0 ? 0 : 1;
}

/* checks for duplicate, and if unique enrich and insert node */
int hsm_action_enqueue(struct state *state,
		       struct hsm_action_item *hai,
		       uint32_t archive_id, uint64_t hal_flags,
		       int64_t timestamp) {
	struct hsm_action_node *han;
	int rc;

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

	han = xcalloc(1, sizeof(struct hsm_action_node));
#ifdef DEBUG_ACTION_NODE
	han->magic = DEBUG_ACTION_NODE;
	CDS_INIT_LIST_HEAD(&han->node);
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
	han->info.data = xstrndup(hai->hai_data, hai->hai_len - sizeof(*hai));
	han->info.archive_id = archive_id;
	han->info.hal_flags = hal_flags;
	han->info.timestamp = timestamp;
	// ignore failure here: the worst that can happen is we'll get
	// order wrong on recovery
	(void)protocol_setjson_int(han->hai, "timestamp", timestamp);

	rc = hsm_action_enqueue_common(state, han);
	// ignore duplicates
	if (rc < 0 && rc != -EEXIST) {
		return rc;
	}

	return rc < 0 ? 0 : 1;
}

/* remove action node from its queue */
void hsm_action_assign(struct hsm_action_queues *queues,
			struct hsm_action_node *han,
			struct client *client) {
#ifdef DEBUG_ACTION_NODE
	assert(han->magic == DEBUG_ACTION_NODE);
#endif
	switch (han->info.action) {
	case HSMA_RESTORE:
		queues->state->stats.pending_restore--;
		queues->state->stats.running_restore++;
		break;
	case HSMA_ARCHIVE:
		queues->state->stats.pending_archive--;
		queues->state->stats.running_archive++;
		break;
	case HSMA_REMOVE:
		queues->state->stats.pending_remove--;
		queues->state->stats.running_remove++;
		break;
	default:
		LOG_ERROR(-EINVAL, "requested to dequeue %s",
			  ct_action2str(han->info.action));
	}

	cds_list_del(&han->node);
	han->client = client;
	cds_list_add_tail(&han->node, &client->active_requests);
}
