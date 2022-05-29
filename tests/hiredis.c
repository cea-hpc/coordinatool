#include <assert.h>
#include <sys/epoll.h>

#include "../copytool/coordinatool.h"

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


enum step {
	STEP_POPULATE,
	STEP_COUNT,
	STEP_CLEAN,
	STEP_DONE
};
struct testdata {
	enum step step;
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

void cb(redisAsyncContext *ac, void *_reply, void *privdata) {
	struct testdata *testdata = privdata;
	int rc;
	redisReply *reply = _reply;

	if (!reply) {
		LOG_INFO("No reply?! %d %s", ac->c.err, ac->c.errstr);
		testdata->stop = true;
		testdata->rc = 1;
		return;
	}
	LOG_DEBUG("step %d count %d", testdata->step, testdata->counter);
	print_reply(reply, -1);


	switch (testdata->step) {
	case STEP_POPULATE:
		if (testdata->counter == 0) {
			// match first del
			// del list could have worked if rpevious test failed
			assert(reply->type == REDIS_REPLY_INTEGER &&
					(reply->integer == 0 || reply->integer == 1));
		} else {
			// lpush returns list length, just count with it
			assert(reply->type == REDIS_REPLY_INTEGER && reply->integer == testdata->counter);
		}

		rc = redisAsyncCommand(ac, cb, privdata,
				"lpush testList %d", testdata->counter);
		if (rc) {
			LOG_ERROR(-EIO, "redisAsyncCommand fail: %d/%d", rc, errno);
			testdata->stop = true;
			testdata->rc = 1;
			return;
		}

		if (testdata->counter++ >= 100) {
			testdata->counter = 0;
			testdata->step = STEP_COUNT;
		}

		break;
	case STEP_COUNT:
		assert(reply->type == REDIS_REPLY_INTEGER && reply->integer == 101);
		testdata->step = STEP_CLEAN;
		rc = redisAsyncCommand(ac, cb, privdata,
				"llen testList");
		if (rc) {
			LOG_ERROR(-EIO, "redisAsyncCommand fail: %d/%d", rc, errno);
			testdata->stop = true;
			testdata->rc = 1;
			return;
		}
		break;
	case STEP_CLEAN:
		if (testdata->counter == 0) {
			// llen
			assert(reply->type == REDIS_REPLY_INTEGER &&
					reply->integer == 101);
		} else {
			// lpush returns list length, just count with it
			assert(reply->type == REDIS_REPLY_STRING && atoi(reply->str) == testdata->counter - 1);
		}
		if (testdata->counter++ > 100) {
			testdata->counter = 0;
			testdata->step = STEP_DONE;
			rc = redisAsyncCommand(ac, cb, privdata,
						"llen testList")
				|| redisAsyncCommand(ac, cb, privdata,
						"del testList");
		} else {
			rc = redisAsyncCommand(ac, cb, privdata,
						"rpop testList");
		}
		if (rc) {
			LOG_ERROR(-EIO, "redisAsyncCommand fail: %d/%d", rc, errno);
			testdata->stop = true;
			testdata->rc = 1;
			return;
		}
		break;
			
	case STEP_DONE:
		switch (testdata->counter) {
		case 0:
			// count left 0
			assert(reply->type == REDIS_REPLY_INTEGER && reply->integer == 0);
			break;
		case 1:
			// del didn't delete anything since list was empty
			assert(reply->type == REDIS_REPLY_INTEGER && reply->integer == 0);
			break;
		}

		if (testdata->counter++ > 0)
			testdata->stop = 1;
		break;
	}
}

#define MAX_EVENTS 10
int main() {
	struct state state;
	struct testdata testdata = { 0 };
	int rc;
	
	llapi_msg_set_level(LLAPI_MSG_INFO);

	state.epoll_fd = epoll_create1(0);
	if (state.epoll_fd < 0) {
		LOG_ERROR(-errno, "could not create epoll fd");
		return 1;
	}

	rc = redis_connect(&state);
	if (rc < 0) {
		LOG_ERROR(rc, "could not redis_connect");
		return 1;
	}

	/* subscribe works, but then any other command fails on the conneiction...
			"subscribe testpubsub");
	 */
	rc = redisAsyncCommand(state.redis_ac, cb, &testdata,
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

	return testdata.rc;
}
