/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "master_ct.h"

void hsm_action_queues_init(struct hsm_action_queues *queues) {
	cds_wfcq_init(&queues->restore_head, &queues->restore_tail);
	cds_wfcq_init(&queues->archive_head, &queues->archive_tail);
	cds_wfcq_init(&queues->remove_head, &queues->remove_tail);
}

int hsm_action_enqueue(struct state *state,
		       struct hsm_action_item *hai) {

	struct hsm_action_queues *queues = &state->queues;
	struct hsm_action_node *node;
       
	if (hai->hai_action == HSMA_CANCEL) {
		/* XXX look through all the lists, for now ignore */
		// memo API: cds_wfcq_for_each_blocking_safe
		return 0;
	}

	node = malloc(sizeof(struct hsm_action_node) +
		      hai->hai_len - sizeof(struct hsm_action_item));
	if (!node) {
		abort();
	}
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
	return 0;
}

struct hsm_action_item *hsm_action_dequeue(struct state *state,
					   enum hsm_copytool_action action) {
	struct hsm_action_queues *queues = &state->queues;
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
