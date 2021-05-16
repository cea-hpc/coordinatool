/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <stdint.h>

#include "coordinatool.h"

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

long parse_int(const char *arg, long max) {
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

int main(int argc, char *argv[]) {
	struct option long_opts[] = {
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet",   no_argument, NULL, 'q' },
		{ "archive", required_argument, NULL, 'A' },
		{ "port", required_argument, NULL, 'p' },
		{ "host", required_argument, NULL, 'H' },
		{ 0 },
	};
	int rc;

	// default options
	int verbose = LLAPI_MSG_INFO;
	struct state state = {
		.host = "::",
		.port = "5123",
		.queues.archive_id = ARCHIVE_ID_UNINIT,
	};
	CDS_INIT_LIST_HEAD(&state.stats.clients);
	CDS_INIT_LIST_HEAD(&state.waiting_clients);

	while ((rc = getopt_long(argc, argv, "vqA:H:p:",
			         long_opts, NULL)) != -1) {
		switch (rc) {
		case 'A':
			if (state.archive_cnt >= LL_HSM_MAX_ARCHIVES_PER_AGENT) {
				LOG_ERROR(-E2BIG, "too many archive id given");
				return EXIT_FAILURE;
			}
			state.archive_id[state.archive_cnt] =
				parse_int(optarg, INT_MAX);
			if (state.archive_id[state.archive_cnt] < 0)
				return EXIT_FAILURE;
			state.archive_cnt++;
			break;
		case 'v':
			verbose++;
			break;
		case 'q':
			verbose--;
			break;
		case 'H':
			state.host = optarg;
			break;
		case 'p':
			state.port = optarg;
			break;
		default:
			return EXIT_FAILURE;
		}
	}
	if (argc != optind + 1) {
		LOG_ERROR(-EINVAL, "no mount point specified");
		return EXIT_FAILURE;
	}
	state.mntpath = argv[optind];

	llapi_msg_set_level(verbose);

	rc = ct_start(&state);
	if (rc)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
