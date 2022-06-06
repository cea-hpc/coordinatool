/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <sys/timerfd.h>

#include "coordinatool.h"

int timer_init(struct state *state) {
	int fd, rc;

	/* use realtime because we'll potentially manipulate restored timestamps
	 * from old actions. */
	fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
	if (fd < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not create timerfd");
		return rc;
	}

	state->timer_fd = fd;

	return epoll_addfd(state->epoll_fd, fd, (void*)(uintptr_t)fd);
}

int timer_rearm(struct state *state) {
	struct itimerspec its = { 0 };
	int64_t closest_ns = INT64_MAX, ns;
	struct cds_list_head *n;
	int rc;

	cds_list_for_each(n, &state->stats.disconnected_clients) {
		struct client *client =
			caa_container_of(n, struct client, node_clients);
		ns = client->disconnected_timestamp
			+ state->config.client_grace_ms * NS_IN_MSEC;
		if (closest_ns > ns)
			closest_ns = ns;
	}

	if (closest_ns == INT64_MAX) {
		return 0;
	}

	ts_from_ns(&its.it_value, closest_ns);
	rc = timerfd_settime(state->timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
	if (rc < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not set timerfd expiration time");
	}
	return rc;
}

void handle_expired_timers(struct state *state) {
	struct cds_list_head *n, *nnext;
	int64_t ns = gettime_ns();

	cds_list_for_each_safe(n, nnext, &state->stats.disconnected_clients) {
		struct client *client =
			caa_container_of(n, struct client, node_clients);
		if (ns < client->disconnected_timestamp
				+ state->config.client_grace_ms * NS_IN_MSEC)
			continue;

		client_disconnect(client);
	}
}
