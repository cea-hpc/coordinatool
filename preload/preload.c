/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "preload.h"

#pragma GCC diagnostic ignored "-Wunused-parameter"

int llapi_hsm_copytool_register(struct hsm_copytool_private **priv,
				const char *mnt, int archive_count,
				int *archives, int rfd_flags) {
	struct hsm_copytool_private *ct;
	int rc = 0;

	ct = xcalloc(sizeof(*ct), 1);

	ct->magic = CT_PRIV_MAGIC;
	CDS_INIT_LIST_HEAD(&ct->actions);
	ct->mnt_fd = ct->open_by_fid_fd = -1;
	ct->state.socket_fd = -1;

	ct->mnt = xstrdup(mnt);

	ct->mnt_fd = open(mnt, O_RDONLY);
	if (ct->mnt_fd < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not open fs root");
		goto err_out;
	}

	ct->open_by_fid_fd = openat(ct->mnt_fd, ".lustre/fid", O_RDONLY);
	if (ct->open_by_fid_fd < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not open .lustre/fid");
		goto err_out;
	}

	rc = ct_config_init(&ct->state.config);
	if (rc)
		goto err_out;

	rc = state_init(ct);
	if (rc)
		goto err_out;

	ct->hal = xmalloc(ct->state.config.hsm_action_list_size);

	rc = tcp_connect(&ct->state);
	if (rc) {
		LOG_ERROR(rc, "Could not connect to server");
		goto err_out;
	}

	*priv = ct;
	return 0;

err_out:
	state_cleanup(ct, true);
	free(ct->hal);
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

	state_cleanup(ct, true);
	free(ct->hal);
	close(ct->mnt_fd);
	close(ct->open_by_fid_fd);
	close(ct->state.socket_fd);
	free(ct->mnt);
	ct_config_free(&ct->state.config);
	free(ct);
	return 0;
}

int llapi_hsm_copytool_recv(struct hsm_copytool_private *ct,
			    struct hsm_action_list **halh, int *msgsize) {
	int rc;

	if (!ct || ct->magic != CT_PRIV_MAGIC || !halh || !msgsize)
		return -EINVAL;

again:
	rc = protocol_request_recv(&ct->state);
	if (rc) {
		LOG_WARN("Sending recv request to server failed with %d. Reconnecting.", rc);
		goto reconnect;
	}


	ct->msgsize = -1;
	while (ct->msgsize == -1) {
		rc = protocol_read_command(ct->state.socket_fd, NULL, copytool_cbs, ct);
		if (rc) {
			LOG_WARN("read from server failed with %d. Reconnecting.", rc);
			goto reconnect;
		}
	}

	*halh = ct->hal;
	*msgsize = ct->msgsize;
	return 0;

reconnect:
	rc = tcp_connect(&ct->state);
	if (rc) {
		LOG_ERROR(rc, "Could not reconnect to server");
		return rc;
	}
	goto again;
}

int llapi_hsm_action_begin(struct hsm_copyaction_private **phcp,
			   const struct hsm_copytool_private *ct,
			   const struct hsm_action_item *hai,
			   int restore_mdt_index, int restore_open_flags,
			   bool is_error) {
	static int (*real_action_begin)(struct hsm_copyaction_private **,
					 const struct hsm_copytool_private *,
					 const struct hsm_action_item *,
					 int, int, bool);
	if (!real_action_begin) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
		real_action_begin = dlsym(RTLD_NEXT, "llapi_hsm_action_begin");
#pragma GCC diagnostic pop
		if (!real_action_begin)
			return -EIO;
	}
	int rc = real_action_begin(phcp, ct, hai, restore_mdt_index,
				   restore_open_flags, is_error);

	if (rc == 0) {
		struct hsm_copyaction_private *hcp;
		hcp = realloc(*phcp, sizeof(*hcp));
		if (!hcp)
			abort();
		/* XXX archive_id */
		hcp->archive_id = ct->state.config.archive_id;
		hcp->cookie = hai->hai_cookie;
		*phcp = hcp;
	}
	return rc;
}


int llapi_hsm_action_end(struct hsm_copyaction_private **phcp,
			 const struct hsm_extent *he, int hp_flags,
			 int errval) {
	static int (*real_action_end)(struct hsm_copyaction_private **,
				       const struct hsm_extent *, int, int);
	if (!real_action_end) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
		real_action_end = dlsym(RTLD_NEXT, "llapi_hsm_action_end");
#pragma GCC diagnostic pop
		if (!real_action_end)
			return -EIO;
	}

	if (!phcp)
		return -EINVAL;

	struct hsm_copyaction_private *hcp = *phcp;
	const struct hsm_copytool_private *ct = hcp->ct_priv;
	uint32_t archive_id = hcp->archive_id;
	uint64_t cookie = hcp->cookie;
	int rc, rc_done;

	rc = real_action_end(phcp, he, hp_flags, errval);
	state_removefile(ct, cookie);

	rc_done = protocol_request_done(&ct->state, archive_id,
					cookie, rc);

	return rc || rc_done;
}
