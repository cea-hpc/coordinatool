/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef PRELOAD_H
#define PRELOAD_H

#include <urcu/list.h>
#include <sys/types.h>
#include <dirent.h>

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
	/* end of lustre's hsm_copytool_private */
	struct cds_list_head actions;
        struct ct_state state;
	struct hsm_action_list *hal;
	int msgsize;
	DIR *client_dir;
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
	/* end of lustre's hsm_copyaction_private */
	uint32_t archive_id;
	uint64_t cookie;
};


/* protocol.c */
extern protocol_read_cb copytool_cbs[];

/* state.c */
int state_init(struct hsm_copytool_private *ct);
void state_cleanup(struct hsm_copytool_private *ct,
		   bool final);
int state_createfile(const struct hsm_copytool_private *ct,
		     struct hsm_action_list *hal, uint64_t cookie,
		     json_t *action_item);
void state_removefile(const struct hsm_copytool_private *ct,
		      uint64_t cookie);

#endif
