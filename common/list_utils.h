/* SPDX-License-Identifier: LGPL-3.0-or-later */

#pragma once

/* compiler.h is included in list.h for newer versions of urcu, but
 * we cannot depend on it yet so just re-include it for caa_container_of */
#include <urcu/compiler.h>
#include <urcu/list.h>

/* get next item from a list of list. Note the list is updated! */
static inline struct cds_list_head *
cds_manylists_next(struct cds_list_head *node, struct cds_list_head ***heads_p)
{
	struct cds_list_head **heads = *heads_p;
	while (node && node->next == heads[0]) {
		heads++;
		node = heads[0];
	}
	*heads_p = heads;
	return node ? node->next : NULL;
}

/* It's a bit of a pain to chain multiple lists in cds_for_each(_safe) manually,
 * so this helper just loops over each lists */
#define cds_manylists_for_each_safe(j, n, nnext, heads) \
	for (j = 0; heads[j]; j++)                      \
	cds_list_for_each_safe(n, nnext, heads[j])

/* count number of items in list */
static inline int cds_list_count(struct cds_list_head *list)
{
	struct cds_list_head *n;
	int count = 0;
	cds_list_for_each(n, list)
	{
		count++;
	}
	return count;
}
