/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>

#include "protocol.h"


/* lustre has a magic check for these two -- keep the magic, but make
 * it different on purpose to make sure we don't mix calls.
 */
#define CT_PRIV_MAGIC 0xC52C9B6F
struct hsm_copytool_private {
        int                              magic;
        char                            *mnt;
        struct kuc_hdr                  *kuch;
        int                              mnt_fd;
        int                              open_by_fid_fd;
        struct lustre_kernelcomm        *kuc;
};

#define CP_PRIV_MAGIC 0x3DA10D41
struct hsm_copyaction_private {
        __u32                                    magic;
        __u32                                    source_fd;
        __s32                                    data_fd;
        const struct hsm_copytool_private       *ct_priv;
        struct hsm_copy                          copy;
        lstatx_t                                 statx;
};

#pragma GCC diagnostic ignored "-Wunused-parameter"

int llapi_hsm_register_event_fifo(const char *path) {
	/* blatantly lie -- can be implemented later if required by
	 * just copying lustre's, unfortunately we can't just get
	 * llapi_hsm_event_fd out of it...
	 * We'll also need llapi_hsm_log_error and
	 * llapi_hsm_unregister_event_fifo at this point
	 * (noop if this is skipped)
	 */
	return 0;
}


int llapi_hsm_copytool_register(struct hsm_copytool_private **priv,
                                const char *mnt, int archive_count,
                                int *archives, int rfd_flags) {
	return -ENOTSUP;
}

int llapi_hsm_copytool_unregister(struct hsm_copytool_private **priv) {
	return -ENOTSUP;
}


int llapi_hsm_copytool_recv(struct hsm_copytool_private *ct,
                            struct hsm_action_list **halh, int *msgsize) {
	return -ENOTSUP;
}

int llapi_hsm_action_begin(struct hsm_copyaction_private **phcp,
                           const struct hsm_copytool_private *ct,
                           const struct hsm_action_item *hai,
                           int restore_mdt_index, int restore_open_flags,
                           bool is_error) {
	return -ENOTSUP;
}

int llapi_hsm_action_end(struct hsm_copyaction_private **phcp,
                         const struct hsm_extent *he, int hp_flags, int errval) {
	return -ENOTSUP;
}

int llapi_hsm_action_progress(struct hsm_copyaction_private *hcp,
                              const struct hsm_extent *he, __u64 total,
                              int hp_flags) {
	/* lie here too -- XXX later */
	return 0;
}

int llapi_hsm_action_get_fd(const struct hsm_copyaction_private *hcp) {
	return -ENOTSUP;
}
