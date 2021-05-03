/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"

void hsm_action_queues_init(struct hsm_action_queues *queues) {
	cds_wfcq_init(&queues->restore_head, &queues->restore_tail);
	cds_wfcq_init(&queues->archive_head, &queues->archive_tail);
	cds_wfcq_init(&queues->remove_head, &queues->remove_tail);
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

struct hsm_action_item *hsm_action_search_queue(struct cds_wfcq_head *head,
						struct cds_wfcq_tail *tail,
						unsigned long cookie,
						bool pop) {
	struct cds_wfcq_node *n, *next;
	struct hsm_action_node *node;

	__cds_wfcq_for_each_blocking_safe(head, tail, n, next) {
		node = caa_container_of(n, struct hsm_action_node, node);
		if (node->hai.hai_cookie != cookie)
			continue;
		if (pop)
			// somehow dequeue/delete?
			return NULL;
		return &node->hai;
	}
	return NULL;
}

int hsm_action_enqueue(struct hsm_action_queues *queues,
		       struct hsm_action_item *hai) {
	struct hsm_action_node *node;
	struct cds_wfcq_head *head;
	struct cds_wfcq_tail *tail;

	switch (hai->hai_action) {
	case HSMA_RESTORE:
		head = &queues->restore_head;
		tail = &queues->restore_tail;
		break;
	case HSMA_ARCHIVE:
		head = &queues->archive_head;
		tail = &queues->archive_tail;
		break;
	case HSMA_REMOVE:
		head = &queues->remove_head;
		tail = &queues->remove_tail;
		break;
	default:
		/* XXX HSMA_CANCEL: caller should try searching all queues */
		return -EINVAL;
	}

	/* XXX try bloom filter first */
	if (hsm_action_search_queue(head, tail, hai->hai_cookie, false))
		return 0;

	node = malloc(sizeof(struct hsm_action_node) +
		      hai->hai_len - sizeof(struct hsm_action_item));
	if (!node) {
		abort();
	}
	cds_wfcq_node_init(&node->node);
	memcpy(&node->hai, hai, hai->hai_len);


	/* are there clients waiting? */
	// XXX
	/* else enqueue */
	cds_wfcq_enqueue(head, tail, &node->node);
	return 1;
}

struct hsm_action_item *hsm_action_dequeue(struct hsm_action_queues *queues,
					   enum hsm_copytool_action action) {
	struct cds_wfcq_node *node;

	switch (action) {
	case HSMA_RESTORE:
		node = __cds_wfcq_dequeue_nonblocking(&queues->restore_head,
						    &queues->restore_tail);
		break;
	case HSMA_ARCHIVE:
		node = __cds_wfcq_dequeue_nonblocking(&queues->archive_head,
						    &queues->archive_tail);
		break;
	case HSMA_REMOVE:
		node = __cds_wfcq_dequeue_nonblocking(&queues->remove_head,
						    &queues->remove_tail);
		break;
	default:
		LOG_ERROR(-EINVAL, "requested to dequeue %s",
			  ct_action2str(action));
		return NULL;
	}

	if (!node)
		return NULL;
	/* blocking case should never happen for us (single thread),
	 * warn and return null */
	if (node == CDS_WFCQ_WOULDBLOCK) {
		LOG_INFO("dequeueing action %s would block, assuming empty",
			 ct_action2str(action));
		return NULL;
	}

	return &caa_container_of(node, struct hsm_action_node, node)->hai;
}
