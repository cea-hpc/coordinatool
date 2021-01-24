/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <stdint.h>
#include <linux/lustre/lustre_idl.h>
#include <sys/socket.h>
#include <netdb.h>


#include "logs.h"

struct state {
	// options
	const char *mntpath;
	int archive_cnt;
	int archive_id[LL_HSM_MAX_ARCHIVES_PER_AGENT];
	const char *host;
	const char *port;
	// states value
	struct hsm_copytool_private *ctdata;
	int epoll_fd;
	int hsm_fd;
	int listen_fd;
};

static inline int epoll_addfd(int epoll_fd, int fd) {
	struct epoll_event ev;
	int rc;

	ev.events = EPOLLIN;
	ev.data.fd = fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not add fd to epoll watches");
		return rc;
	}

	return 0;
}

int tcp_listen(struct state *state) {
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int sfd, s;
	int rc;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	s = getaddrinfo(state->host, state->port, &hints, &result);
	if (s != 0) {
		/* getaddrinfo does not use errno, cheat with debug */
		LOG_DEBUG("ERROR getaddrinfo: %s\n", gai_strerror(s));
		return -EIO;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype,
				rp->ai_protocol);
		if (sfd == -1)
			continue;

		if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;                  /* Success */

		close(sfd);
	}

	if (rp == NULL) {
		rc = -errno;
		LOG_ERROR(rc, "Could not bind tcp server");
		return rc;
	}

	freeaddrinfo(result);

	rc = listen(sfd, 10);
	if (rc < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not listen");
		return rc;
	}
	state->listen_fd = sfd;
	rc = epoll_addfd(state->epoll_fd, sfd);
	if (rc < 0) {
		LOG_ERROR(rc, "Could not add listen socket to epoll");
		return rc;
	}

	return 0;
}

static inline char *sockaddr2str(struct sockaddr_storage *addr, socklen_t len) {
	char host[NI_MAXHOST], service[NI_MAXSERV];
	int rc;

	rc = getnameinfo((struct sockaddr*)addr, len,
			 host, sizeof(host),
			 service, sizeof(service),
			 NI_NUMERICSERV);
	if (rc != 0) {
		LOG_DEBUG("ERROR getnameinfo: %s", gai_strerror(rc));
		return NULL;
	}

	char *addrstring;
	asprintf(&addrstring, "%s:%s", host, service);

	return addrstring;
}


int handle_client_connect(struct state *state) {
	int fd, rc;
	struct sockaddr_storage peer_addr;
	socklen_t peer_addr_len = sizeof(peer_addr);

	fd = accept(state->listen_fd, (struct sockaddr*)&peer_addr,
		    &peer_addr_len);
	if (fd < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not accept connection");
		return rc;
	}

	char *peer_str = sockaddr2str(&peer_addr, peer_addr_len);

	LOG_DEBUG("Got client connection from %s", peer_str);
	free(peer_str);
	return 0;
}

int handle_ct_event(struct hsm_copytool_private *ctdata) {
	struct hsm_action_list *hal;
	int msgsize, rc;

	rc = llapi_hsm_copytool_recv(ctdata, &hal, &msgsize);
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
	LOG_DEBUG("copytool fs=%s, archive#=%d, item_count=%d",
			hal->hal_fsname, hal->hal_archive_id,
			hal->hal_count);
	// XXX match fsname with known one?

	struct hsm_action_item *hai = hai_first(hal);
	unsigned int i = 0;
	while (++i <= hal->hal_count) {
		struct lu_fid fid;

		/* memcpy to avoid unaligned accesses */
		memcpy(&fid, &hai->hai_fid, sizeof(fid));
		LOG_DEBUG("item %d: %s on "DFID ,
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

	rc = epoll_addfd(state->epoll_fd, state->hsm_fd);
	if (rc < 0) {
		LOG_ERROR(rc, "could not add hsm fd to epoll");
		return rc;
	}

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

	rc = tcp_listen(state);
	if (rc < 0)
		return rc;

	rc = ct_register(state);
	if (rc < 0)
		return rc;

	while (1) {
		nfds = epoll_wait(state->epoll_fd, events, MAX_EVENTS, -1);
		if (nfds < 0) {
			rc = -errno;
			LOG_ERROR(rc, "epoll_wait failed");
			return rc;
		}
		int n;
		for (n = 0; n < nfds; n++) {
			if (events[n].data.fd == state->hsm_fd) {
				handle_ct_event(state->ctdata);
			} else if (events[n].data.fd == state->listen_fd) {
				handle_client_connect(state);
			} else {
				// client request
			}
		}


	}
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
		{ "host", required_argument, NULL, 'h' },
		{ 0 },
	};
	int rc;

	// default options
	int verbose = LLAPI_MSG_INFO;
	struct state state = {
		.host = "::",
		.port = "5123",
	};

	while ((rc = getopt_long(argc, argv, "vqA:h:p:",
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
		case 'h':
			state.host = optarg;
			break;
		case 'p':
			state.port = optarg;
			break;
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
