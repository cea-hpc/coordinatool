/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netdb.h>

#include "client.h"
#include "lustre.h"

int tcp_connect(struct state *state) {
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int sfd, s;
	int rc;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	s = getaddrinfo(state->config.host, state->config.port, &hints, &result);
	if (s != 0) {
		/* getaddrinfo does not use errno, cheat with debug */
		LOG_DEBUG("ERROR getaddrinfo: %s", gai_strerror(s));
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
		LOG_ERROR(rc, "Could not connect to %s:%s", state->config.host, state->config.port);
		return rc;
	}

	freeaddrinfo(result);
	state->socket_fd = sfd;

	return 0;
}

int parse_hai_cb(struct hsm_action_item *hai, unsigned int archive_id,
		 unsigned long flags, void *arg) {
	struct active_requests_state *active_requests = arg;

	if (active_requests->archive_id == 0) {
		active_requests->archive_id = archive_id;
	} else if (active_requests->archive_id != archive_id) {
		LOG_ERROR(-EINVAL, "Only support one archive_id active for now (got %d and %d)",
			  active_requests->archive_id, archive_id);
		return -EINVAL;
	}
	if (active_requests->flags == 0) {
		active_requests->flags = flags;
	} else if (active_requests->flags != flags) {
		LOG_ERROR(-EINVAL, "Only support one active flagsfor now (got %lx and %lx)",
			  active_requests->flags, flags);
		return -EINVAL;
	}

	json_array_append_new(active_requests->hai_list, json_hsm_action_item(hai));

#if 0
	/* XXX send every 10000 items or so to avoid starving resources */
	if (json_array_size(active_requests->hai_list) >= 10000) {
		struct state *state = containers_of(active_requests...)
		protocol_request_queue(state->socket_fd, active_requests);
		protocol_read_command(state->socket_fd, protocol_cbs, state);
	}
#endif
	return 0;
}

int client(struct state *state) {
	int rc;

	rc = tcp_connect(state);
	if (rc < 0)
		return rc;

	if (state->config.send_queue) {
		state->active_requests.hai_list = json_array();
		if (!state->active_requests.hai_list)
			abort();
		// XXX fsname
		strcpy(state->active_requests.fsname, "testfs0");
		rc = parse_active_requests(0, parse_hai_cb,
					   &state->active_requests);
		if (rc < 0)
			return rc;

		if (json_array_size(state->active_requests.hai_list) == 0) {
			LOG_DEBUG("Nothing to enqueue, exiting");
			return 0;
		}
		protocol_request_queue(state->socket_fd,
				       &state->active_requests);
		protocol_read_command(state->socket_fd, protocol_cbs, state);
		return 0;
	}

	protocol_request_status(state->socket_fd);
	protocol_request_recv(state->socket_fd, state);
	protocol_read_command(state->socket_fd, protocol_cbs, state);
	protocol_read_command(state->socket_fd, protocol_cbs, state);

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
		.config.host = "::1",
		.config.port = "5123",
		.config.max_restore = -1,
		.config.max_archive = -1,
		.config.max_remove = -1,
		.config.hsm_action_list_size = 1024*1024,
	};

	while ((rc = getopt_long(argc, argv, "vqH:p:Q",
			         long_opts, NULL)) != -1) {
		switch (rc) {
		case 'v':
			verbose++;
			break;
		case 'q':
			verbose--;
			break;
		case 'H':
			state.config.host = optarg;
			break;
		case 'p':
			state.config.port = optarg;
			break;
		case 'Q':
			state.config.send_queue = true;
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
