/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef PRELOAD_H
#define PRELOAD_H

#include <urcu/list.h>
#include "protocol.h"
#include "client_common.h"
#include "utils.h"

/* preload.c */
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
	struct cds_list_head actions;
        struct ct_state state;
	struct hsm_action_list *hal;
	int msgsize;
	/* pipe used to signal request has been processed to recv thread
	 * or process. Since some copytools fork to handle work we cannot
	 * just reply directly safely from llapi_hsm_action_end */
	int notify_done_fd[2];
};

/* this one is used as is by llapi so keep the same magic,
 * but happend our cookie at the end for ourselves
 */
#define CP_PRIV_MAGIC 0x19880429
struct hsm_copyaction_private {
	__u32 magic;
	__u32 source_fd;
	__s32 data_fd;
	const struct hsm_copytool_private *ct_priv;
	struct hsm_copy copy;
	lstatx_t statx;
	uint32_t archive_id;
	uint64_t cookie;
};


/* done infos */
struct notify_done {
	uint64_t cookie;
	int archive_id;
	int rc;
};

/* protocol.c */
extern protocol_read_cb copytool_cbs[];

#endif
