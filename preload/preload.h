/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef PRELOAD_H
#define PRELOAD_H

#include <urcu/list.h>
#include "protocol.h"
#include "client_common.h"
#include "utils.h"

/* preload.c */
struct action_key {
	uint64_t cookie;
	struct lu_fid dfid;
};

/* remember running actions to send back on reconnect */
struct action_tree_node {
	struct action_key key;
	json_t *hai;
};

/* lustre has a magic check for these two -- keep the magic, but make
 * it different on purpose to make sure we don't mix calls.
 * keep start of struct in sync with lustre's to use as is with llapi
 * action helpers
 */
#define CT_PRIV_MAGIC 0xC52C9B6F
struct hsm_copytool_private {
	unsigned int magic;
	char *mnt;
	void *kuch;
	int mnt_fd;
	int open_by_fid_fd;
	void *kuc;
	void *actions_tree;
	struct ct_state state;
	struct hsm_action_list *hal;
	int msgsize;
	/* pipe used to signal request has been processed to recv thread
	 * or process. Since some copytools fork to handle work we cannot
	 * just reply directly safely from llapi_hsm_action_end */
	int notify_done_fd[2];
};

/* this one is used as is by llapi so keep the same magic,
 * but happend our key at the end for ourselves
 */
/* hsm_copy is variable sized and not at the end, but it's the same
 * in lustre_hsm.c: they don't copy the data part of the action item */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#define CP_PRIV_MAGIC 0x19880429
struct hsm_copyaction_private {
	__u32 magic;
	__u32 source_fd;
	__s32 data_fd;
	const struct hsm_copytool_private *ct_priv;
	struct hsm_copy copy;
	lstatx_t statx;
	struct action_key key;
};
#pragma clang diagnostic pop

/* done infos */
struct notify_done {
	struct action_key key;
	int rc;
};

/* protocol.c */
extern protocol_read_cb copytool_cbs[];

/* tree.c */
void action_insert(struct hsm_copytool_private *ct,
		   struct action_tree_node *node);
void action_delete(struct hsm_copytool_private *ct, struct action_key *key);
json_t *actions_get_list(struct hsm_copytool_private *ct);

#endif
