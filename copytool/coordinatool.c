/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <stdint.h>

#include "coordinatool.h"
#include "version.h"

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

int epoll_delfd(int epoll_fd, int fd) {
	int rc = 0;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not remove fd from epoll watches");
	}
	return rc;
}

static long parse_int(const char *arg, long max) {
	long rc;
	char *endptr;

	rc = strtol(arg, &endptr, 0);
	if (rc < 0 || rc > max) {
		rc = -ERANGE;
		LOG_ERROR(rc, "argument %s too big", arg);
	}
	if (*endptr != '\0') {
		rc = -EINVAL;
		LOG_ERROR(rc, "argument %s contains (trailing) garbage", arg);
	}
	return rc;
}

static void print_help(char *argv0) {
	printf("Usage: %s [options] mountpoint\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("    -v, --verbose: increase verbosity (repeatable)\n");
	printf("    -q, --quiet: decrease verbosity\n");
	printf("    -A, --archive: set which archive id to handle\n");
	printf("    -p, --port: select port to listen to\n");
	printf("    -H, --host: select address to listen to\n");
	printf("    -V, --version: print version info\n");
	printf("    -h, --help: this help\n");
}

static void print_version(void) {
	printf("Coordinatool version %s\n", VERSION);
}

#define MAX_EVENTS 10
static int ct_start(struct state *state) {
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

	rc = redis_connect(state);
	if (rc < 0)
		return rc;

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
				LOG_INFO("%d on error/hup", events[n].data.fd);
			}
			if (events[n].data.fd == state->hsm_fd) {
				handle_ct_event(state);
			} else if (events[n].data.fd == state->listen_fd) {
				handle_client_connect(state);
			} else if (events[n].data.ptr == state->redis_ac) {
				if (events[n].events & EPOLLIN)
					redisAsyncHandleRead(state->redis_ac);
				// EPOLLOUT is only requested when we have something
				// to send
				if (events[n].events & EPOLLOUT)
					redisAsyncHandleWrite(state->redis_ac);
			} else {
				struct client *client = events[n].data.ptr;
				if (protocol_read_command(client->fd, client->id, client,
							  protocol_cbs, state) < 0) {
					free_client(state, client);
				}
			}
		}


	}
}

#define OPT_REDIS_HOST 257
#define OPT_REDIS_PORT 258
int main(int argc, char *argv[]) {
	struct option long_opts[] = {
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet",   no_argument, NULL, 'q' },
		{ "archive", required_argument, NULL, 'A' },
		{ "port", required_argument, NULL, 'p' },
		{ "host", required_argument, NULL, 'H' },
		{ "redis-host", required_argument, NULL, OPT_REDIS_HOST },
		{ "redis-port", required_argument, NULL, OPT_REDIS_PORT },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ 0 },
	};
	int rc;

	// state init
	struct state state = {
		.queues.archive_id = ARCHIVE_ID_UNINIT,
	};
	CDS_INIT_LIST_HEAD(&state.stats.clients);
	CDS_INIT_LIST_HEAD(&state.waiting_clients);

	config_init(&state.config);

	while ((rc = getopt_long(argc, argv, "A:vqH:p:Vh",
			         long_opts, NULL)) != -1) {
		switch (rc) {
		case 'A':
			if (state.archive_cnt >= LL_HSM_MAX_ARCHIVES_PER_AGENT) {
				LOG_ERROR(-E2BIG, "too many archive id given");
				rc = EXIT_FAILURE;
				goto out;
			}
			state.archive_id[state.archive_cnt] =
				parse_int(optarg, INT_MAX);
			if (state.archive_id[state.archive_cnt] < 0) {
				rc = EXIT_FAILURE;
				goto out;
			}
			state.archive_cnt++;
			break;
		case 'v':
			state.config.verbose++;
			llapi_msg_set_level(state.config.verbose);
			break;
		case 'q':
			state.config.verbose--;
			llapi_msg_set_level(state.config.verbose);
			break;
		case 'H':
			free((void*)state.config.host);
			state.config.host = xstrdup(optarg);
			break;
		case 'p':
			free((void*)state.config.port);
			state.config.port = xstrdup(optarg);
			break;
		case OPT_REDIS_HOST:
			free((void*)state.config.redis_host);
			state.config.redis_host = xstrdup(optarg);
			break;
		case OPT_REDIS_PORT:
			state.config.redis_port = parse_int(optarg, 65535);
			break;
		case 'V':
			print_version();
			rc = EXIT_SUCCESS;
			goto out;
		case 'h':
			print_help(argv[0]);
			rc = EXIT_SUCCESS;
			goto out;
		default:
			fprintf(stderr, "Unknown option %c, see --help\n", rc);
			rc = EXIT_FAILURE;
			goto out;
		}
	}
	if (argc != optind + 1) {
		LOG_ERROR(-EINVAL, "no mount point specified");
		rc = EXIT_FAILURE;
		goto out;
	}
	state.mntpath = argv[optind];

	rc = ct_start(&state);
	rc = rc ? EXIT_FAILURE : EXIT_SUCCESS;

out:
	redisAsyncDisconnect(state.redis_ac);
	config_free(&state.config);
	return rc;
}
