/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <sys/epoll.h>
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

	struct hsm_action_queues *queues =
		hsm_action_queues_get(state,
				      hal->hal_archive_id,
				      hal->hal_flags,
				      hal->hal_fsname);
	if (!queues)
		return -EINVAL;

	struct hsm_action_item *hai = hai_first(hal);
	unsigned int i = 0;
	while (++i <= hal->hal_count) {
		if ((rc = hsm_action_enqueue(queues, hai) < 0))
			return rc;

		struct lu_fid fid;

		/* memcpy to avoid unaligned accesses */
		memcpy(&fid, &hai->hai_fid, sizeof(fid));
		LOG_INFO("enqueued (%d): %s on "DFID ,
				i, ct_action2str(hai->hai_action),
				PFID(&fid));
		hai = hai_next(hai);
	}
	return hal->hal_count;
}

int ct_register(struct state *state) {
	int rc;

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

#define MAX_EVENTS 10
int ct_start(struct state *state) {
	int rc;
	struct epoll_event events[MAX_EVENTS];
	int nfds;

	state->epoll_fd = epoll_create1(0);
	if (state->epoll_fd < 0) {
		rc = -errno;
		LOG_ERROR(rc, "could not create epoll fd");
		return rc;
	}

	hsm_action_queues_init(state, &state->queues);

	rc = tcp_listen(state);
	if (rc < 0)
		return rc;

	rc = ct_register(state);
	if (rc < 0)
		return rc;

	LOG_NORMAL("Starting main loop");
	while (1) {
		nfds = epoll_wait(state->epoll_fd, events, MAX_EVENTS, -1);
		if (nfds < 0 && errno == EINTR)
			continue;
		if (nfds < 0) {
			rc = -errno;
			LOG_ERROR(rc, "epoll_wait failed");
			return rc;
		}
		int n;
		for (n = 0; n < nfds; n++) {
			if (events[n].events & (EPOLLERR|EPOLLHUP)) {
				LOG_INFO("%d on error/hup\n", events[n].data.fd);
			}
			if (events[n].data.fd == state->hsm_fd) {
				handle_ct_event(state);
			} else if (events[n].data.fd == state->listen_fd) {
				handle_client_connect(state);
			} else {
				struct client *client = events[n].data.ptr;
				if (protocol_read_command(client->fd, client,
							  protocol_cbs, state) < 0) {
					free_client(state, client);
				}
			}
		}
	}
}
