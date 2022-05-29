/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <sys/epoll.h>

#include "coordinatool.h"

static void redis_addwrite(void *_state) {
	struct state *state = _state;
	struct epoll_event ev;

	ev.events = EPOLLIN|EPOLLOUT;
	ev.data.ptr = state->redis_ac;

	int rc = epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD,
			   state->redis_ac->c.fd, &ev);
	if (rc < 0)
		LOG_WARN(-errno, "Could not listen redis fd for write: redis broken!");
}

static void redis_delwrite(void *_state) {
	struct state *state = _state;
	struct epoll_event ev;

	ev.events = EPOLLIN;
	ev.data.ptr = state->redis_ac;

	int rc = epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD,
			   state->redis_ac->c.fd, &ev);
	if (rc < 0)
		LOG_WARN(-errno, "Could not stop listening redis fd for write?!");
}

static int redis_error_to_errno(int err) {
	switch (err) {
	case REDIS_ERR_OOM:
		return ENOMEM;
	case REDIS_ERR_EOF:
	case REDIS_ERR_PROTOCOL:
		return EPROTO;
#ifdef REDIS_ERR_TIMEOUT
	/* timeout has been added in 1.0.0, not in epel8 yet */
	case REDIS_ERR_TIMEOUT:
		return ETIMEDOUT;
#endif
	case REDIS_ERR_IO:
		/* .h says to use errno */
		if (errno)
			return errno;
		// fallthrough
	default:
		return EIO;
	}
}

int redis_connect(struct state *state) {
	int rc;
	redisAsyncContext *ac;

	// XXX options for redis ip/port
	ac = redisAsyncConnect("127.0.0.1", 6379);
	if (!ac)
	       abort(); // ENOMEM
	if (ac->err) {
		LOG_ERROR(-redis_error_to_errno(ac->err),
			  "redis error on connect: %s",
			  ac->errstr ? ac->errstr : "No info available");
		return -1;
	}
	state->redis_ac = ac;

	/* We have our own event loop, so we need to register our own callbacks...
	 * Switch to libev(ent) at some point?
	 * For now it's just as easy to use directly... */
	ac->ev.data = state;
	ac->ev.addWrite = redis_addwrite;
	ac->ev.delWrite = redis_delwrite;
	// delRead never used
	// addRead could be used for epoll oneshot, but we're not oneshot
	// cleanup only used on disconnect, we can do it ourselves
	// scheduleTimeout could be useful though...
	//   if that is useful switching to libev sounds better than adding
	//   a timerfd

	rc = epoll_addfd(state->epoll_fd, state->redis_ac->c.fd,
			 state->redis_ac);
	if (rc < 0)
		return rc;

	return 0;
}
