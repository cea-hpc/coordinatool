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
	const char *host;
	const char *port;
	// states value
	int socket_fd;
};

int tcp_connect(struct state *state) {
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int sfd, s;
	int rc;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

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

		if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;                  /* Success */

		close(sfd);
	}

	if (rp == NULL) {
		rc = -errno;
		LOG_ERROR(rc, "Could not connect to %s:%s", state->host, state->port);
		return rc;
	}

	freeaddrinfo(result);
	state->socket_fd = sfd;

	return 0;
}

int client(struct state *state) {
	int rc;

	rc = tcp_connect(state);
	if (rc < 0)
		return rc;

	return 0;
}

int main(int argc, char *argv[]) {
	struct option long_opts[] = {
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet",   no_argument, NULL, 'q' },
		{ "port", required_argument, NULL, 'p' },
		{ "host", required_argument, NULL, 'H' },
		{ 0 },
	};
	int rc;

	// default options
	int verbose = LLAPI_MSG_INFO;
	struct state state = {
		.host = "::1",
		.port = "5123",
	};

	while ((rc = getopt_long(argc, argv, "vqH:p:",
			         long_opts, NULL)) != -1) {
		switch (rc) {
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
	if (argc != optind) {
		LOG_ERROR(-EINVAL, "extra argument specified");
		return EXIT_FAILURE;
	}

	llapi_msg_set_level(verbose);

	rc = client(&state);
	if (rc)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
