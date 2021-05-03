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

int hsm_action_enqueue(struct hsm_action_queues *queues,
		       struct hsm_action_item *hai) {
	struct hsm_action_node *node;

	if (hai->hai_action == HSMA_CANCEL) {
		/* XXX look through all the lists, for now ignore */
		// memo API: cds_wfcq_for_each_blocking_safe
		return 0;
	}

	/* XXX check duplicate */

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
	switch (hai->hai_action) {
	case HSMA_RESTORE:
		cds_wfcq_enqueue(&queues->restore_head, &queues->restore_tail,
				 &node->node);
		break;
	case HSMA_ARCHIVE:
		cds_wfcq_enqueue(&queues->archive_head, &queues->archive_tail,
				 &node->node);
		break;
	case HSMA_REMOVE:
		cds_wfcq_enqueue(&queues->remove_head, &queues->remove_tail,
				 &node->node);
		break;
	default:
		return -EINVAL;
	}
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
