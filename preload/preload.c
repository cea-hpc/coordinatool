/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>

#include "protocol.h"
#include "client_common.h"
#include "preload.h"


/* lustre has a magic check for these two -- keep the magic, but make
 * it different on purpose to make sure we don't mix calls.
 */
#define CT_PRIV_MAGIC 0xC52C9B6F
struct hsm_copytool_private {
        unsigned int magic;
	struct cds_list_head actions;
        struct ct_state state;
	struct hsm_action_list *hal;
	int msgsize;
};

#define CP_PRIV_MAGIC 0x3DA10D41
struct hsm_copyaction_private {
        unsigned int magic;
	struct hsm_copytool_private *ct;
	unsigned long long cookie;
	int fd;
	struct cds_list_head list;
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
	struct hsm_copytool_private *ct = calloc(sizeof(*ct), 1);
	if (!ct)
		return -ENOMEM;

	ct->magic = CT_PRIV_MAGIC;
	CDS_INIT_LIST_HEAD(&ct->actions);

	int rc = ct_connect(&ct->state, mnt, archive_count, archives,
			    rfd_flags);
	if (rc) {
		free(ct);
		return rc;
	}

	*priv = ct;
	return 0;
}

int llapi_hsm_copytool_unregister(struct hsm_copytool_private **priv) {
	if (!priv)
		return -EINVAL;

	struct hsm_copytool_private *ct = *priv;
	if (ct->magic != CT_PRIV_MAGIC)
		return -EINVAL;

	free(ct->hal);
	return ct_disconnect(&ct->state);
}

int llapi_hsm_copytool_recv(struct hsm_copytool_private *ct,
                            struct hsm_action_list **halh, int *msgsize) {
	int rc;

	if (!ct || ct->magic != CT_PRIV_MAGIC || !halh || !msgsize)
		return -EINVAL;

	rc = ct_request_recv(&ct->state);
	if (rc)
		return rc;

	ct->msgsize = -1;
	rc = ct_read_command(&ct->state, copytool_cbs, ct, &ct->msgsize);
	if (rc)
		return rc;

	*halh = ct->hal;
	*msgsize = ct->msgsize;
	return 0;
}

int llapi_hsm_action_begin(struct hsm_copyaction_private **phcp,
                           const struct hsm_copytool_private *const_ct,
                           const struct hsm_action_item *hai,
                           int restore_mdt_index, int restore_open_flags,
                           bool is_error) {
        struct hsm_copytool_private *ct = (struct hsm_copytool_private *)const_ct;
	int rc;

	if (!ct || ct->magic != CT_PRIV_MAGIC || !hai)
		return -EINVAL;

	struct hsm_copyaction_private *hcp = calloc(sizeof(*hcp), 1);
	hcp->magic = CP_PRIV_MAGIC;
	hcp->ct = ct;
	hcp->cookie = hai->hai_cookie;

	cds_list_add(&hcp->list, &ct->actions);
	rc = ct_request_start(&ct->state, hcp->cookie, restore_mdt_index,
			      restore_open_flags, is_error);
	if (rc)
		goto err;

	rc = ct_read_command(&ct->state, copytool_cbs, ct, &hcp->fd);
	if (rc)
		goto err;
	if (hcp->fd < 0) {
		rc = hcp->fd;
		goto err;
	}

	return 0;
err:
	free(hcp);
	return rc;
}

int llapi_hsm_action_end(struct hsm_copyaction_private **phcp,
                         const struct hsm_extent *he, int hp_flags, int errval) {
	if (!phcp)
		return -EINVAL;
	struct hsm_copyaction_private *hcp = *phcp;
	if (!hcp || hcp->magic != CP_PRIV_MAGIC)
		return -EINVAL;

	return ct_request_done(&hcp->ct->state, hcp->cookie,
			       he, hp_flags, errval);
}

int llapi_hsm_action_progress(struct hsm_copyaction_private *hcp,
                              const struct hsm_extent *he, __u64 total,
                              int hp_flags) {
	/* lie here too -- XXX later */
	return 0;
}

int llapi_hsm_action_get_fd(const struct hsm_copyaction_private *hcp) {
	if (!hcp || hcp->magic != CP_PRIV_MAGIC)
		return -EINVAL;

	return hcp->fd;
}
