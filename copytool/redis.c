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

static void redis_disconnect_cb(const struct redisAsyncContext *ac,
				int status UNUSED) {
	struct state *state = ac->data;

	LOG_WARN(0, "Redis disconnected");
	state->redis_ac = NULL;
}

int redis_connect(struct state *state) {
	int rc;
	redisAsyncContext *ac;

	// allow running without redis if host is empty
	if (state->config.redis_host[0] == 0)
		return 0;

	ac = redisAsyncConnect(state->config.redis_host,
			       state->config.redis_port);
	if (!ac)
	       abort(); // ENOMEM
	if (ac->err) {
		LOG_ERROR(-redis_error_to_errno(ac->err),
			  "redis error on connect: %s",
			  ac->errstr[0] ? ac->errstr : "No info available");
		return -1;
	}
	state->redis_ac = ac;

	/* hiredis provides a data field that's not used internally,
	 * use it for disconnect cleanup as that doesn't pass any argument */
	ac->data = state;
	redisAsyncSetDisconnectCallback(ac, redis_disconnect_cb);

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

static void cb_common(redisAsyncContext *ac, redisReply *reply,
		      const char *action, uint64_t cookie) {
	if (!reply) {
		LOG_WARN(-EIO, "Redis error in callback! %d: %s", ac->c.err,
			 ac->c.errstr[0] ? ac->c.errstr : "Error string not set");
		LOG_WARN(-EIO, "Could not %s cookie %lx", action, cookie);
		redisAsyncDisconnect(ac);
		return;
	}
}

static void cb_insert(redisAsyncContext *ac, void *_reply, void *private) {
	uint64_t cookie = (uint64_t)private;
	redisReply *reply = _reply;

	cb_common(ac, reply, "insert", cookie);


	// should be an int with 1 or 0 depending on if we created a new key,
	// but we don't really care so not checking.
}

static void cb_delete(redisAsyncContext *ac, void *_reply, void *private) {
	/* note: it'd be more user-friendly to print fid here, but by the time
	 * callback is called the han has been freed.
	 * It'd be possible to delay han free to this function, but it is
	 * useful to have the coordinatool work even if redis is down
	 */
	uint64_t cookie = (uint64_t)private;
	redisReply *reply = _reply;

	cb_common(ac, reply, "delete", cookie);

	if (reply->type != REDIS_REPLY_INTEGER || reply->integer != 1)
		LOG_WARN(-ENOENT,
			 "Redis did not delete key we tried to delete for cookie %lx",
			 cookie);
}

int redis_insert(struct state *state, struct hsm_action_node *han) {
	if (!state->redis_ac)
		return 0;
	char *hai_json_str = json_dumps(han->hai, JSON_COMPACT);
	int rc = redisAsyncCommand(state->redis_ac, cb_insert,
				   (void*)han->info.cookie,
				   "hset coordinatool_requests %lx %s",
				   han->info.cookie, hai_json_str);
	free(hai_json_str);
	if (rc) {
		LOG_WARN(-EIO, "Redis error trying to set "DFID,
			 PFID(&han->info.dfid));
		return -EIO;
	}
	return 0;
}

int redis_delete(struct state *state, uint64_t cookie) {
	if (!state->redis_ac)
		return 0;
	int rc = redisAsyncCommand(state->redis_ac, cb_delete, (void*)cookie,
				   "hdel coordinatool_requests %lx",
				   cookie);
	if (rc) {
		LOG_WARN(-EIO, "Redis error trying to delete %lx", cookie);
		return -EIO;
	}
	return 0;
}
