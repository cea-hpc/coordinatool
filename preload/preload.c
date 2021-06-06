/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "protocol.h"
#include "client_common.h"
#include "preload.h"


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
};

#pragma GCC diagnostic ignored "-Wunused-parameter"

int llapi_hsm_copytool_register(struct hsm_copytool_private **priv,
                                const char *mnt, int archive_count,
                                int *archives, int rfd_flags) {
	struct hsm_copytool_private *ct = calloc(sizeof(*ct), 1);
	int rc = 0;
	if (!ct)
		return -ENOMEM;

	ct->magic = CT_PRIV_MAGIC;
	CDS_INIT_LIST_HEAD(&ct->actions);
	ct->mnt_fd = ct->open_by_fid_fd = -1;

	ct->mnt = strdup(mnt);
	if (!ct->mnt) {
		rc = -ENOMEM;
		goto err_out;
	}

	ct->mnt_fd = open(mnt, O_RDONLY);
	if (ct->mnt_fd < 0) {
		rc = -errno;
		goto err_out;
	}

	ct->open_by_fid_fd = openat(ct->mnt_fd, ".lustre/fid", O_RDONLY);
	if (ct->open_by_fid_fd < 0) {
		rc = -errno;
		goto err_out;
	}

	rc = ct_config_init(&ct->state.config);
	if (rc)
		goto err_out;

	rc = tcp_connect(&ct->state);
	if (rc)
		goto err_out;

	*priv = ct;
	return 0;

err_out:
	if (ct->mnt_fd >= 0)
		close(ct->mnt_fd);
	if (ct->open_by_fid_fd)
		close(ct->open_by_fid_fd);
	free(ct->mnt);
	free(ct);
	return rc;
}

int llapi_hsm_copytool_unregister(struct hsm_copytool_private **priv) {
	if (!priv)
		return -EINVAL;

	struct hsm_copytool_private *ct = *priv;
	if (ct->magic != CT_PRIV_MAGIC)
		return -EINVAL;

	free(ct->hal);
	close(ct->mnt_fd);
	close(ct->open_by_fid_fd);
	close(ct->state.socket_fd);
	free(ct->mnt);
	free(ct);
	return 0;
}

int llapi_hsm_copytool_recv(struct hsm_copytool_private *ct,
                            struct hsm_action_list **halh, int *msgsize) {
	int rc;

	if (!ct || ct->magic != CT_PRIV_MAGIC || !halh || !msgsize)
		return -EINVAL;

	rc = protocol_request_recv(&ct->state);
	if (rc)
		return rc;

	ct->msgsize = -1;
	while (ct->msgsize == -1) {
		rc = protocol_read_command(ct->state.socket_fd, ct, copytool_cbs, NULL);
		if (rc)
			return rc;
	}

	*halh = ct->hal;
	*msgsize = ct->msgsize;
	return 0;
}
