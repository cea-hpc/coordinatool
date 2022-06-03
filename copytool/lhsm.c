/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <limits.h>

#include "coordinatool.h"

int handle_ct_event(struct state *state) {
	struct hsm_action_list *hal;
	int msgsize, rc;

	rc = llapi_hsm_copytool_recv(state->ctdata, &hal, &msgsize);
	if (rc == -ESHUTDOWN) {
		LOG_INFO("shutting down");
		return 0;
	}
	if (rc < 0) {
		LOG_ERROR(rc, "Could not recv hsm message");
		return rc;
	}
	if (hal->hal_count > INT_MAX) {
		rc = -E2BIG;
		LOG_ERROR(rc, "got too many events at once (%u)",
			  hal->hal_count);
		return rc;
	}
	if (hal->hal_version != HAL_VERSION) {
		rc = -EINVAL;
		LOG_ERROR(rc, "received hsm action list version %d, expecting %d",
			  hal->hal_version, HAL_VERSION);
		abort();
	}
	LOG_DEBUG("copytool fs=%s, archive#=%d, item_count=%d",
			hal->hal_fsname, hal->hal_archive_id,
			hal->hal_count);

	if (strcmp(hal->hal_fsname, state->fsname)) {
		LOG_ERROR(-EINVAL, "Got unexpected fsname from lustre ct event: expected %s got %s. Accepting anyway.",
			  state->fsname, hal->hal_fsname);
	}

	struct hsm_action_item *hai = hai_first(hal);
	unsigned int i = 0;
	int64_t timestamp = gettime_ns();
	while (++i <= hal->hal_count) {
		if ((rc = hsm_action_enqueue(state, hai,
					     hal->hal_archive_id,
					     hal->hal_flags,
					     timestamp) < 0))
			return rc;

		struct lu_fid fid;

		/* memcpy to avoid unaligned accesses */
		memcpy(&fid, &hai->hai_fid, sizeof(fid));
		LOG_INFO("enqueued (%d): %s on "DFID ,
				i, ct_action2str(hai->hai_action),
				PFID(&fid));
		hai = hai_next(hai);
	}
	ct_schedule(state);

	return hal->hal_count;
}

int ct_register(struct state *state) {
	int rc;
	char fsname[LUSTRE_MAXFSNAME + 1];

	rc = llapi_search_fsname(state->mntpath, fsname);
	if (rc < 0) {
		LOG_ERROR(rc, "cannot find a Lustre filesystem mounted at '%s'",
			  state->mntpath);
		return rc;
	}
	state->fsname = xstrdup(fsname);

	rc = llapi_hsm_copytool_register(&state->ctdata, state->mntpath,
					 state->archive_cnt, state->archive_id, 0);
	if (rc < 0) {
		LOG_ERROR(rc, "cannot start copytool interface");
		return rc;
	}

	state->hsm_fd = llapi_hsm_copytool_get_fd(state->ctdata);
	if (state->hsm_fd < 0) {
		LOG_ERROR(state->hsm_fd,
			  "cannot get kuc fd after hsm registration");
		return state->hsm_fd;
	}

	rc = epoll_addfd(state->epoll_fd, state->hsm_fd, (void*)(uintptr_t)state->hsm_fd);
	if (rc < 0) {
		LOG_ERROR(rc, "could not add hsm fd to epoll");
		return rc;
	}

	LOG_INFO("Registered lustre copytool");
	return 0;
}
