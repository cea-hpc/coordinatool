/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <search.h>
#include <urcu/compiler.h>

#include "preload.h"

static int tree_compare(const void *a, const void *b) {
	const struct action_key *va = a, *vb = b;

	if (va->cookie < vb->cookie)
		return -1;
	if (va->cookie > vb->cookie)
		return 1;
	return memcmp(&va->dfid, &vb->dfid, sizeof(va->dfid));
}

void action_insert(struct hsm_copytool_private *ct,
		   struct action_tree_node *node) {
	struct action_key **tree_key;

	tree_key = tsearch(&node->key, &ct->actions_tree,
			   tree_compare);
	if (!tree_key)
		abort();
	if (*tree_key != &node->key) {
		/* duplicate */
		json_decref(node->hai);
		free(node);
		return;
	}
}

void action_delete(struct hsm_copytool_private *ct, struct action_key *key) {
	if (!tdelete(key, &ct->actions_tree, tree_compare))
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
		caa_container_of(*(void **)nodep, struct action_tree_node, key);

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

	if (json_array_size(hai_list) == 0) {
		/* nothing in progress */
		json_decref(hai_list);
		hai_list = NULL;
	}

	return hai_list;
}
