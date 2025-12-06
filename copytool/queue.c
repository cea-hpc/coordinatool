/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>
#include <search.h>

#include "config.h"
#include "coordinatool.h"

void hsm_action_queues_init(struct hsm_action_queues *queues)
{
	CDS_INIT_LIST_HEAD(&queues->waiting_restore);
	CDS_INIT_LIST_HEAD(&queues->waiting_archive);
	CDS_INIT_LIST_HEAD(&queues->waiting_remove);
}

struct cds_list_head *get_queue_list(struct hsm_action_queues *queues,
				     struct hsm_action_node *han)
{
	switch (han->info.action) {
	case HSMA_RESTORE:
		return &queues->waiting_restore;
	case HSMA_ARCHIVE:
		return &queues->waiting_archive;
	case HSMA_REMOVE:
		return &queues->waiting_remove;
	default:
		LOG_ERROR(-EINVAL,
			  "han %lx for " DFID
			  " was neither restore, archive nor remove: %#x",
			  han->info.cookie, PFID(&han->info.dfid),
			  han->info.action);
		return NULL;
	}
}

static int tree_compare(const void *a, const void *b)
{
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

static void _hsm_action_free(struct hsm_action_node *han, bool final_cleanup)
{
#ifdef DEBUG_ACTION_NODE
	assert(han->magic == DEBUG_ACTION_NODE);
#endif
	LOG_DEBUG("freeing han for " DFID " node %p", PFID(&han->info.dfid),
		  (void *)&han->node);
	if (!final_cleanup) {
		redis_delete_request(han->info.cookie, &han->info.dfid);
		cds_list_del(&han->node);
#if HAVE_PHOBOS
		free(han->info.hsm_fuid);
#endif
		if (!tdelete(&han->info, &state->hsm_actions_tree,
			     tree_compare))
			abort();
	}
	free((void *)han->info.data);
	report_free_action(han);
	if (han->hai)
		json_decref(han->hai);
	free(han);
}

void hsm_action_free(struct hsm_action_node *han)
{
	_hsm_action_free(han, false);
}

static void tree_free_cb(void *nodep)
{
	struct item_info *item_info =
		caa_container_of(nodep, struct item_info, cookie);
	struct hsm_action_node *han =
		caa_container_of(item_info, struct hsm_action_node, info);

	_hsm_action_free(han, true);
}

void hsm_action_free_all(void)
{
	tdestroy(state->hsm_actions_tree, tree_free_cb);
}

struct hsm_action_node *hsm_action_search(unsigned long cookie,
					  struct lu_fid *dfid)
{
	struct item_info search_item = {
		.cookie = cookie,
		.dfid = *dfid,
	};
	void *key = tfind(&search_item, &state->hsm_actions_tree, tree_compare);

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
int hsm_action_enqueue(struct hsm_action_node *han, struct cds_list_head *list)
{
	if (!list)
		list = hsm_action_node_schedule(han);
	if (!list)
		list = get_queue_list(&state->queues, han);
	if (!list) {
		/* We're losing track of the action here, free it.
		 * (it should never happen, get queue list logged error)
		 * free expects a list in good shape */
		CDS_INIT_LIST_HEAD(&han->node);
		hsm_action_free(han);
		return -EINVAL;
	}

	/* bookkeeping:
	 * - was running if han->client is set
	 * - otherwise was only already pending if han->node is zero (fresh calloc)
	 */
	bool was_running = (han->client != NULL);
	bool was_pending = !was_running && (han->node.next != NULL ||
					    cds_list_empty(&han->node));

	if (was_running) {
		/* unset client anyway, we're pending now! */
		han->client = NULL;
		redis_deassign_request(han);
	}

	switch (han->info.action) {
	case HSMA_RESTORE:
		if (!was_pending)
			state->stats.pending_restore++;
		if (was_running)
			state->stats.running_restore--;
		break;
	case HSMA_ARCHIVE:
		if (!was_pending)
			state->stats.pending_archive++;
		if (was_running)
			state->stats.running_archive--;
		break;
	case HSMA_REMOVE:
		if (!was_pending)
			state->stats.pending_remove++;
		if (was_running)
			state->stats.running_remove--;
		break;
	default:
		return -EINVAL;
	}

#ifdef DEBUG_ACTION_NODE
	assert(han->magic == DEBUG_ACTION_NODE);
	LOG_DEBUG("Inserting han for " DFID " node %p in %p",
		  PFID(&han->info.dfid), (void *)&han->node, (void *)list);
#endif
	// XXX insert at timestamp-appropriate position in list
	// - if timestamp later than tail add last
	// - otherwise start comparing from head
	cds_list_add_tail(&han->node, list);
	return 1;
}

static int hsm_action_new_common(struct hsm_action_node *han)
{
	struct item_info **tree_key;

	tree_key = tsearch(&han->info, &state->hsm_actions_tree, tree_compare);
	if (!tree_key)
		abort();
	if (*tree_key != &han->info) {
		/* duplicate */
		free((void *)han->info.data);
		json_decref(han->hai);
		free(han);
		return -EEXIST;
	}

	report_new_action(han);

#if HAVE_PHOBOS
	(void)phobos_enrich(han);
#endif
	redis_store_request(han);

	return hsm_action_enqueue(han, NULL);
}

int hsm_action_new_json(json_t *json_hai, int64_t timestamp,
			struct hsm_action_node **han_out, const char *requestor)
{
	struct hsm_action_node *han;
	struct hsm_action_item hai;
	const char *data;
	int rc;

	rc = json_hsm_action_item_get(json_hai, &hai, sizeof(hai), &data);
	if (rc) {
		LOG_WARN(rc, "%s: Could not process invalid hai: skipping",
			 requestor);
		return rc;
	}

	han = xcalloc(1, sizeof(struct hsm_action_node));
#ifdef DEBUG_ACTION_NODE
	han->magic = DEBUG_ACTION_NODE;
	CDS_INIT_LIST_HEAD(&han->node);
#endif
	han->info.cookie = hai.hai_cookie;
	han->info.action = hai.hai_action;
	han->info.dfid = hai.hai_dfid;
	han->info.hai_len = hai.hai_len;
	han->info.archive_id =
		protocol_getjson_int(json_hai, "hal_archive_id", 0);
	han->info.hal_flags = protocol_getjson_int(json_hai, "hal_flags", 0);
	if (!han->info.archive_id) {
		LOG_WARN(-EINVAL, "%s: hai did not contain archive_id",
			 requestor);
		free(han);
		return -EINVAL;
	}
	switch (hai.hai_action) {
	case HSMA_RESTORE:
	case HSMA_ARCHIVE:
	case HSMA_REMOVE:
		break;
	default:
		LOG_WARN(rc, "%s: hai had invalid action %d\n", requestor,
			 hai.hai_action);
		free(han);
		return -EINVAL;
	}

	han->info.timestamp = protocol_getjson_int(json_hai, "timestamp", 0);
	if (!han->info.timestamp) {
		han->info.timestamp = timestamp;
		(void)protocol_setjson_int(json_hai, "timestamp", timestamp);
	}

	// allocations last
	han->info.data = xmemdup0(data, han_data_len(han));
	han->hai = json_incref(json_hai);

	rc = hsm_action_new_common(han);
	if (rc < 0 && rc != -EEXIST) {
		return rc;
	}

	if (han_out) {
		*han_out = (rc == -EEXIST ? NULL : han);
	}

	return rc < 0 ? 0 : 1;
}

/* checks for duplicate, and if unique enrich and insert node */
int hsm_action_new_lustre(struct hsm_action_item *hai, uint32_t archive_id,
			  uint64_t hal_flags, int64_t timestamp)
{
	struct hsm_action_node *han;
	int rc;

	if (hai->hai_action == HSMA_CANCEL) {
		han = hsm_action_search(hai->hai_cookie, &hai->hai_dfid);
		if (!han) {
			LOG_WARN(-ENOENT, "Received cancel for " DFID " / %llx, not in queue -- just done?",
				 PFID(&hai->hai_dfid), hai->hai_cookie);
			return 0;
		}
		report_action(han, "cancel " DFID, PFID(&han->info.dfid));
		if (!han->client) {
			/* not running - just depop */
			hsm_action_free(han);
			/* nothing else to do (apparently don't need to report to lustre) */
			return 0;
		}
		// XXX signal cancel to client -- most if not all of the copytool
		// implementations currently ignore cancels, so this is left for
		// later
		LOG_DEBUG("Ignored cancel for " DFID " / %llx currently on client %s (%d)",
			  PFID(&hai->hai_dfid), hai->hai_cookie, han->client->id,
			  han->client->fd);
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
	han->info.cookie = hai->hai_cookie;
	han->info.action = hai->hai_action;
	/* XXX check usage of hai_fid vs. hai_dfid (in particular for logs,
	 * we logged hai_fid in lhsm.c... */
	han->info.dfid = hai->hai_dfid;
	han->info.hai_len = hai->hai_len;
	han->info.data = xmemdup0(hai->hai_data, han_data_len(han));
	han->info.archive_id = archive_id;
	han->info.hal_flags = hal_flags;
	han->info.timestamp = timestamp;
	// ignore failure here: the worst that can happen is we'll get
	// order wrong on recovery
	(void)protocol_setjson_int(han->hai, "timestamp", timestamp);

	rc = hsm_action_new_common(han);
	// ignore duplicates
	if (rc < 0 && rc != -EEXIST) {
		return rc;
	}

	return rc < 0 ? 0 : 1;
}

void hsm_action_start(struct hsm_action_node *han, struct client *client)
{
#ifdef DEBUG_ACTION_NODE
	assert(han->magic == DEBUG_ACTION_NODE);
	LOG_DEBUG("%s (%d): moving node %p to %p (active requests)", client->id,
		  client->fd, (void *)han, (void *)&client->active_requests);
#endif

	/* same as hsm_action_enqueue */
	bool was_running = (han->client != NULL);
	bool was_pending = !was_running && (han->node.next != NULL ||
					    cds_list_empty(&han->node));

	switch (han->info.action) {
	case HSMA_RESTORE:
		if (was_pending)
			state->stats.pending_restore--;
		if (!was_running) {
			state->stats.running_restore++;
			client->current_restore++;
		}
		break;
	case HSMA_ARCHIVE:
		if (was_pending)
			state->stats.pending_archive--;
		if (!was_running) {
			state->stats.running_archive++;
			client->current_archive++;
		}
		break;
	case HSMA_REMOVE:
		if (was_pending)
			state->stats.pending_remove--;
		if (!was_running) {
			state->stats.running_remove++;
			client->current_remove++;
		}
		break;
	default:
		LOG_ERROR(-EINVAL,
			  "starting han %lx for " DFID
			  " was neither restore, archive nor remove: %#x",
			  han->info.cookie, PFID(&han->info.dfid),
			  han->info.action);
	}

	redis_assign_request(client, han);
	cds_list_del(&han->node);
	han->client = client;
	cds_list_add_tail(&han->node, &client->active_requests);
}
