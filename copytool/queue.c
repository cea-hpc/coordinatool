/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"

void hsm_action_queues_init(struct hsm_action_queues *queues) {
	CDS_INIT_LIST_HEAD(&queues->waiting_restore);
	CDS_INIT_LIST_HEAD(&queues->waiting_archive);
	CDS_INIT_LIST_HEAD(&queues->waiting_remove);
}

void queue_node_free(struct hsm_action_item *hai) {
	struct hsm_action_node *node = caa_container_of(hai, struct hsm_action_node, hai);
	free(node);
}


struct hsm_action_queues *hsm_action_queues_get(struct state *state,
						unsigned int archive_id,
						unsigned long long flags,
						const char *fsname) {
	if (archive_id == ARCHIVE_ID_UNINIT || !fsname) {
		LOG_ERROR(-EINVAL, "No archive_id or fsname received");
		return NULL;
	}

	if (state->queues.archive_id == ARCHIVE_ID_UNINIT) {
		// XXX we only support one archive_id, first one we sees
		// determines what others should be.
		state->queues.archive_id = archive_id;
		state->queues.fsname = strdup(fsname);
		state->queues.hal_flags = flags;
	} else {
		// XXX strcmp fsname?
		if (state->queues.archive_id != archive_id) {
			LOG_ERROR(-EINVAL, "received action list for archive id %d but already seen %d, ignoring these",
				  archive_id, state->queues.archive_id);
			return NULL;
		}
		if (state->queues.hal_flags != flags) {
			LOG_ERROR(-EINVAL, "received action list with different flags (got %llx, expected %llx); keeping current flags anyway",
				  flags, state->queues.hal_flags);
			return NULL;
		}
	}

	return &state->queues;
}

struct hsm_action_item *hsm_action_search_queue(struct cds_list_head *head,
						unsigned long cookie,
						bool pop) {
	struct cds_list_head *n, *next;
	struct hsm_action_node *node;

	cds_list_for_each_safe(n, next, head) {
		node = caa_container_of(n, struct hsm_action_node, node);
		if (node->hai.hai_cookie != cookie)
			continue;
		if (pop) {
			cds_list_del(n);
		}
		return &node->hai;
	}
	return NULL;
}

int hsm_action_enqueue(struct hsm_action_queues *queues,
		       struct hsm_action_item *hai) {
	struct hsm_action_node *node;
	struct cds_list_head *head;

	switch (hai->hai_action) {
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
		/* XXX HSMA_CANCEL: caller should try searching all queues w/ pop and free */
		return -EINVAL;
	}

	/* XXX try bloom filter first, or hash table? */
	if (hsm_action_search_queue(head, hai->hai_cookie, false))
		return 0;

	node = malloc(sizeof(struct hsm_action_node) +
		      hai->hai_len - sizeof(struct hsm_action_item));
	if (!node) {
		abort();
	}
	memcpy(&node->hai, hai, hai->hai_len);


	/* are there clients waiting? */
	// XXX
	/* else enqueue */
	cds_list_add_tail(&node->node, head);
	return 1;
}

struct hsm_action_item *hsm_action_dequeue(struct hsm_action_queues *queues,
					   enum hsm_copytool_action action) {
	struct cds_list_head *node = NULL;

	switch (action) {
	case HSMA_RESTORE:
		if (cds_list_empty(&queues->waiting_restore))
			return NULL;
		node = queues->waiting_restore.next;
		break;
	case HSMA_ARCHIVE:
		if (cds_list_empty(&queues->waiting_archive))
			return NULL;
		node = queues->waiting_archive.next;
		break;
	case HSMA_REMOVE:
		if (cds_list_empty(&queues->waiting_remove))
			return NULL;
		node = queues->waiting_remove.next;
		break;
	default:
		LOG_ERROR(-EINVAL, "requested to dequeue %s",
			  ct_action2str(action));
		return NULL;
	}

	cds_list_del(node);
	return &caa_container_of(node, struct hsm_action_node, node)->hai;
}
