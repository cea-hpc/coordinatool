/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <search.h>
#include <urcu/compiler.h>

#include "preload.h"

static int tree_compare(const void *a, const void *b) {
	__u64 va = *(__u64*)a, vb = *(__u64*)b;

	if (va < vb)
		return -1;
	else if (va == vb)
		return 0;
	return 1;
}

void action_insert(struct hsm_copytool_private *ct,
		   struct action_tree_node *node) {
	uint64_t **tree_key;

	tree_key = tsearch(&node->cookie, &ct->actions_tree,
			   tree_compare);
	if (!tree_key)
		abort();
	if (*tree_key != &node->cookie) {
		/* duplicate */
		json_decref(node->hai);
		free(node);
		return;
	}
}

void action_delete(struct hsm_copytool_private *ct, uint64_t cookie) {
	if (!tdelete(&cookie, &ct->actions_tree, tree_compare))
		abort();
}


/* would want to use twalk_r but that was introduced in glibc 2.30
 * which is not yet available widely enough.
 * Note the symbol is not actually defined anywhere, so this code
 * has not been tested
 */
#ifdef HAVE_TWALK_R
static void walk_append(const void *nodep, VISIT which, void *arg) {
	json_t *hai_list = arg;
#else
static json_t *walk_hai_list;
static void walk_append(const void *nodep, VISIT which, int depth UNUSED) {
	json_t *hai_list = walk_hai_list;
#endif
	
	/* only process internal nodes once */
	switch (which) {
	case preorder:
	case endorder:
		return;
	default:
		break;
	}
	struct action_tree_node *node =
		caa_container_of(*(void **)nodep, struct action_tree_node, cookie);

	if (json_array_append(hai_list, node->hai) < 0)
		abort();
}

json_t *actions_get_list(struct hsm_copytool_private *ct) {
	json_t *hai_list = json_array();
	if (!hai_list)
		abort();

#ifdef HAVE_TWALK_R
	twalk_r(ct->actions_tree, walk_append, hai_list);
#else
	walk_hai_list = hai_list;
	twalk(ct->actions_tree, walk_append);
#endif

	return hai_list;
}
