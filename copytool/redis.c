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
		return -ENOMEM;
	case REDIS_ERR_EOF:
	case REDIS_ERR_PROTOCOL:
		return -EPROTO;
#ifdef REDIS_ERR_TIMEOUT
	/* timeout has been added in 1.0.0, not in epel8 yet */
	case REDIS_ERR_TIMEOUT:
		return -ETIMEDOUT;
#endif
	case REDIS_ERR_IO:
		/* .h says to use errno */
		if (errno)
			return -errno;
		// fallthrough
	default:
		return -EIO;
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
		LOG_ERROR(redis_error_to_errno(ac->err),
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
	if (reply->type == REDIS_REPLY_ERROR) {
		LOG_WARN(-EIO, "Redis error in callback! %s", reply->str);
		LOG_WARN(-EIO, "Could not %s cookie %lx", action, cookie);
		/* do not disconnect in this case: could be bogus request */
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

static int redis_insert(struct state *state, const char *hash,
			uint64_t cookie, const char *data) {
	int rc;

	if (!state->redis_ac)
		return 0;

	rc = redisAsyncCommand(state->redis_ac, cb_insert, (void*)cookie,
			       "hset %s %lx %s", hash, cookie, data);
	if (rc) {
		rc = redis_error_to_errno(rc);
		LOG_WARN(rc, "Redis error trying to set cookie %lx in %s",
			 cookie, hash);
		return rc;
	}
	return 0;
}


static void cb_delete(redisAsyncContext *ac, void *_reply, void *private) {
	uint64_t cookie = (uint64_t)private;
	redisReply *reply = _reply;

	cb_common(ac, reply, "delete", cookie);

	// should be an int with 1 or 0 depending on if key was really found
	// but in the hsm cancel case we'll try to delete from assigned table
	// without checking if it's in: skip check
}

static int redis_delete(struct state *state, const char *hash,
			uint64_t cookie) {
	int rc;

	if (!state->redis_ac)
		return 0;

	rc = redisAsyncCommand(state->redis_ac, cb_delete, (void*)cookie,
			       "hdel %s %lx", hash, cookie);
	if (rc) {
		rc = redis_error_to_errno(rc);
		LOG_WARN(rc, "Redis error trying to delete cookie %lx in %s",
			 cookie, hash);
		return rc;
	}
	return 0;
}

int redis_store_request(struct state *state,
		        struct hsm_action_node *han) {
	char *hai_json_str;
	int rc;

	if (!state->redis_ac)
		return 0;

	hai_json_str = json_dumps(han->hai, JSON_COMPACT);
	if (!hai_json_str) {
		LOG_WARN(-EINVAL, "Could not dump hsm action item to json ("DFID")",
			 PFID(&han->info.dfid));
	}

	rc = redis_insert(state, "coordinatool_requests",
			  han->info.cookie, hai_json_str);
	free(hai_json_str);

	return rc;
}

int redis_assign_request(struct state *state, struct client *client,
			 struct hsm_action_node *han) {
	return redis_insert(state, "coordinatool_assigned",
			    han->info.cookie, client->id);
}

int redis_delete_request(struct state *state, uint64_t cookie) {
	// note we won't bother sending second request if first failed
	// as that likely means connection is broken
	return redis_delete(state, "coordinatool_requests", cookie)
		|| redis_delete(state, "coordinatool_assigned", cookie);
}


struct redis_wait {
	struct state *state;
	int done;
	const char *hash;
	int (*cb)(struct state *state, const char *key, const char *value);
};

static int redis_wait_done(struct state *state, int *done) {
	/* when this function runs the epoll loop is not live yet,
	 * so run our own.
	 * Unfortunately we cannot just use sync functions with ac->c... */
	int rc;
	struct epoll_event event;
	int nfds;

	while (!(*done)) {
		nfds = epoll_wait(state->epoll_fd, &event, 1, -1);
		if ((nfds < 0 && errno == EINTR) || nfds == 0)
			continue;
		if (nfds < 0) {
			rc = -errno;
			LOG_ERROR(rc, "epoll_wait failed");
			return rc;
		}
		if (event.events & (EPOLLERR|EPOLLHUP)) {
			LOG_INFO("%d on error/hup", event.data.fd);
		}
		if (event.data.ptr != state->redis_ac) {
			LOG_ERROR(-EINVAL, "fd other than redis fd ready?! Giving up");
			return -EINVAL;
		}
		if (event.events & EPOLLIN)
			redisAsyncHandleRead(state->redis_ac);
		// EPOLLOUT is only requested when we have something
		// to send
		if (event.events & EPOLLOUT)
			redisAsyncHandleWrite(state->redis_ac);
	}

	return *done;
}

static void redis_scan_cb(redisAsyncContext *ac, void *_reply, void *private) {
	redisReply *reply = _reply;
	struct redis_wait *wait = private;
	int rc;

	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		LOG_ERROR(-EIO, "redis error on setup: %s",
			  reply ? reply->str : ac->c.errstr);
		wait->done = -EIO;
		return;
	}

	// scan reply should be an array with
	// - next cursor
	// - array of alternating key, values
	// and stops when cursor is 0 again. can have duplicates.

	if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
		LOG_ERROR(-EINVAL, "unexpected reply to hscan, type %d elements %ld",
			  reply->type, reply->elements);
		wait->done = -EINVAL;
		return;
	}

	redisReply *cursor_reply = reply->element[0];
	if (cursor_reply->type != REDIS_REPLY_STRING) {
		LOG_ERROR(-EINVAL, "unexpected cursor_reply to hscan, cursor wasn't int but %d",
			  cursor_reply->type);
		wait->done = -EINVAL;
		return;
	}

	reply = reply->element[1];
	if (reply->type != REDIS_REPLY_ARRAY || reply->elements % 2 != 0) {
		LOG_ERROR(-EINVAL, "unexpected reply for hscan values, type %d elements %ld",
			  reply->type, reply->elements);
		wait->done = -EINVAL;
		return;
	}
	for (unsigned int i=0; i < reply->elements; i += 2) {
		redisReply *key_reply = reply->element[i];
		redisReply *value_reply = reply->element[i+1];
		if (key_reply->type != REDIS_REPLY_STRING
		    || value_reply->type != REDIS_REPLY_STRING) {
			LOG_ERROR(-EINVAL, "unexpected reply for hscan values items, type %d / %d",
				  key_reply->type, value_reply->type);
		}
		const char *key = key_reply->str;
		const char *value = value_reply->str;
		rc = wait->cb(wait->state, key, value);
		if (rc) {
			wait->done = rc;
			return;
		}
	}

	const char *cursor = cursor_reply->str;
	if (!strcmp(cursor, "0")) {
		wait->done = 1;
	} else {
		rc = redisAsyncCommand(ac, redis_scan_cb, wait,
				       "hscan %s %s", wait->hash, cursor);
		if (rc) {
			rc = redis_error_to_errno(rc);
			LOG_ERROR(rc, "Redis error trying to scan %s from %s",
				  wait->hash, cursor);
			wait->done = rc;
			return;
		}
	}
}

static int redis_scan_requests(struct state *state,
			       const char *key UNUSED,
			       const char *value) {
	int rc;
	json_error_t json_error;
	json_t *json_hai;


	json_hai = json_loads(value, 0, &json_error);
	if (!json_hai) {
		LOG_ERROR(-EINVAL, "Invalid json from redis (%s): %s",
			  value, json_error.text);
		return -EINVAL;
	}

	rc = hsm_action_enqueue_json(state, json_hai, 0, NULL);
	if (rc < 0)
		return rc;
	return 0;
}

int redis_recovery(struct state *state) {
	struct redis_wait wait = {
		.state = state,
		.cb = redis_scan_requests,
		.hash = "coordinatool_requests",
	};

	int rc = redisAsyncCommand(state->redis_ac, redis_scan_cb, &wait,
				   "hscan %s %d", wait.hash, 0);
	if (rc) {
		rc = redis_error_to_errno(rc);
		LOG_ERROR(rc, "Redis error trying to scan %s (start)",
			  wait.hash);
		return rc;
	}
	rc = redis_wait_done(state, &wait.done);
	if (rc < 0)
		return rc;

	return 0;
}
