/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>
#include <sys/epoll.h>

#include "../copytool/coordinatool.h"

/* fill in dummy requirements to copytool/redis.c */
int hsm_action_enqueue_json(struct state *state UNUSED, json_t *json UNUSED,
			    int64_t timestamp UNUSED,
			    struct hsm_action_node **han_out UNUSED,
			    const char *requestor UNUSED) {
	return 0;
}
struct hsm_action_node *hsm_action_search_queue(struct hsm_action_queues *queues UNUSED,
                                                unsigned long cookie UNUSED,
						struct lu_fid *dfid UNUSED) {
	return NULL;
}
struct client *client_new_disconnected(struct state *state UNUSED,
				       const char *id UNUSED) {
	return NULL;
}
void hsm_action_assign(struct hsm_action_queues *queues UNUSED,
                        struct hsm_action_node *han UNUSED,
			struct client *client UNUSED) {
	return;
}


/* copy from copytool/coordinatool.c */
int epoll_addfd(int epoll_fd, int fd, void *data) {
	struct epoll_event ev;
	int rc;

	ev.events = EPOLLIN;
	ev.data.ptr = data;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not add fd to epoll watches");
		return rc;
	}

	return 0;
}


struct testdata {
	int counter;
	bool stop;
	int rc;
};


void print_reply(redisReply *reply, int idx) {
	switch (reply->type) {
	case REDIS_REPLY_ARRAY:
		LOG_DEBUG("(%d) got array size %ld", idx, reply->elements);
		for (unsigned int i = 0; i < reply->elements; i++) {
			print_reply(reply->element[i], i);
		}
		break;
	case REDIS_REPLY_STRING:
		LOG_DEBUG("(%d) got string %*s", idx, (int)reply->len, reply->str);
		break;
	case REDIS_REPLY_INTEGER:
		LOG_DEBUG("(%d) got int %lld", idx, reply->integer);
		break;
	case REDIS_REPLY_NIL:
		LOG_DEBUG("(%d) got nil", idx);
		break;
	default:
		LOG_DEBUG("(%d) Got reply type %d", idx, reply->type);
	}

}

int cb_common(redisAsyncContext *ac, redisReply *reply,
	       struct testdata *testdata, const char *step) {
	if ((uintptr_t)reply < 4096) {
		LOG_INFO("No reply?! %lx %d %s", (uintptr_t)reply, ac->c.err, ac->c.errstr);
		testdata->stop = true;
		testdata->rc = 1;
		return 1;
	}
	LOG_DEBUG("step %s count %d", step, testdata->counter);
	print_reply(reply, -1);
	return 0;
}

#define REDIS_SEND_CHECK(_ac, _cb, _td, _fmt, ...) do { \
	int _rc = redisAsyncCommand(_ac, _cb, _td, _fmt, ## __VA_ARGS__); \
	if (_rc) { \
		LOG_ERROR(-EIO, "redisAsyncCommand %s fail: %d/%d", _fmt, _rc, errno); \
		_td->stop = true; \
		_td->rc = 1; \
		return; \
	} \
} while (0)

void cb_initial_delete(redisAsyncContext *ac, void *_reply, void *privdata);
void cb_populate(redisAsyncContext *ac, void *_reply, void *privdata);
void cb_llen(redisAsyncContext *ac, void *_reply, void *privdata);
void cb_rpop(redisAsyncContext *ac, void *_reply, void *privdata);
void cb_llen_final(redisAsyncContext *ac, void *_reply, void *privdata);
void cb_del(redisAsyncContext *ac, void *_reply, void *privdata);

void cb_initial_delete(redisAsyncContext *ac, void *_reply, void *privdata) {
	struct testdata *testdata = privdata;
	redisReply *reply = _reply;

	if (cb_common(ac, reply, testdata, "initial delete"))
		return;

	// del list could have worked if previous test failed
	assert(reply->type == REDIS_REPLY_INTEGER &&
	       (reply->integer == 0 || reply->integer == 1));

	REDIS_SEND_CHECK(ac, cb_populate, testdata,
			"lpush testList 0");
}

#define TEST_ITEM_COUNT 100
void cb_populate(redisAsyncContext *ac, void *_reply, void *privdata) {
	struct testdata *testdata = privdata;
	redisReply *reply = _reply;

	if (cb_common(ac, reply, testdata, "populate"))
		return;

	testdata->counter++;

	// lpush returns list length, just count with it
	assert(reply->type == REDIS_REPLY_INTEGER
	       && reply->integer == testdata->counter);

	if (testdata->counter < TEST_ITEM_COUNT) {
		REDIS_SEND_CHECK(ac, cb_populate, testdata,
				 "lpush testList %d", testdata->counter);
	} else {
		testdata->counter = 0;
		REDIS_SEND_CHECK(ac, cb_llen, testdata,
				 "llen testList");
	}
}

void cb_llen(redisAsyncContext *ac, void *_reply, void *privdata) {
	struct testdata *testdata = privdata;
	redisReply *reply = _reply;

	if (cb_common(ac, reply, testdata, "llen"))
		return;

	assert(reply->type == REDIS_REPLY_INTEGER && reply->integer == TEST_ITEM_COUNT);
	REDIS_SEND_CHECK(ac, cb_rpop, testdata,
			 "rpop testList");
}

void cb_rpop(redisAsyncContext *ac, void *_reply, void *privdata) {
	struct testdata *testdata = privdata;
	redisReply *reply = _reply;

	if (cb_common(ac, reply, testdata, "rpop"))
		return;
	// rpop returns last element, which was first to have been inserted
	// (== counter)
	// it's returned as a string.
	assert(reply->type == REDIS_REPLY_STRING
	       && atoi(reply->str) == testdata->counter);
	testdata->counter++;
	if (testdata->counter < TEST_ITEM_COUNT) {
		REDIS_SEND_CHECK(ac, cb_rpop, testdata,
				 "rpop testList");
	} else {
		REDIS_SEND_CHECK(ac, cb_llen_final, testdata,
				 "llen testList");
		REDIS_SEND_CHECK(ac, cb_del, testdata,
				 "del testList");
	}
}

void cb_llen_final(redisAsyncContext *ac, void *_reply, void *privdata) {
	struct testdata *testdata = privdata;
	redisReply *reply = _reply;

	if (cb_common(ac, reply, testdata, "llen final"))
		return;

	// count left 0
	assert(reply->type == REDIS_REPLY_INTEGER && reply->integer == 0);

	// nothing to send, we previously sent two in parallel.
}

void cb_del(redisAsyncContext *ac, void *_reply, void *privdata) {
	struct testdata *testdata = privdata;
	redisReply *reply = _reply;

	if (cb_common(ac, reply, testdata, "del"))
		return;

	// del didn't delete anything since list gets deleted on last pop
	assert(reply->type == REDIS_REPLY_INTEGER && reply->integer == 0);

	// test is over
	testdata->stop = 1;
}

#define MAX_EVENTS 10
int main(void) {
	struct state state;
	struct testdata testdata = { 0 };
	int rc;

	/* if debugging this, increase log level here to LLAPI_MSG_DEBUG */
	llapi_msg_set_level(LLAPI_MSG_INFO);

	state.epoll_fd = epoll_create1(0);
	if (state.epoll_fd < 0) {
		LOG_ERROR(-errno, "could not create epoll fd");
		return 1;
	}

	// XXX take from env like coordinatool config or something
	state.config.redis_host = "localhost";
	state.config.redis_port = 6379;
	rc = redis_connect(&state);
	if (rc < 0) {
		LOG_ERROR(rc, "could not redis_connect");
		return 1;
	}


	/* note subscribe works, but then any other command fails as
	 * subscribe "reserves" the connection: "subscribe testpubsub"
	 * We'd need to reconnect on a dedicated connection for it.
	 */
	/* this will start the callback chain for tests */
	rc = redisAsyncCommand(state.redis_ac, cb_initial_delete, &testdata,
			"del testList");
	if (rc) {
		LOG_ERROR(-EIO, "redisAsyncCommand failed: %d/%d", rc, errno);
		return 1;
	}

	struct epoll_event events[MAX_EVENTS];
	int nfds;
	while (! testdata.stop) {
		nfds = epoll_wait(state.epoll_fd, events, MAX_EVENTS, -1);
		if (nfds < 0 && errno == EINTR)
			continue;
		if (nfds < 0) {
			rc = -errno;
			LOG_ERROR(rc, "epoll_wait failed");
			return 1;
		}
		int n;
		for (n = 0; n < nfds; n++) {
			if (events[n].events & (EPOLLERR|EPOLLHUP)) {
				LOG_INFO("%d on error/hup", events[n].data.fd);
			}
			assert(events[n].data.ptr == state.redis_ac);

			if (events[n].events & EPOLLIN)
				redisAsyncHandleRead(state.redis_ac);
			// EPOLLOUT is only requested when we have something
			// to send
			if (events[n].events & EPOLLOUT)
				redisAsyncHandleWrite(state.redis_ac);
		}
	}

	redisAsyncDisconnect(state.redis_ac);
	return testdata.rc;
}
