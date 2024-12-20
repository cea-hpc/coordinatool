/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <sys/socket.h>
#include <netdb.h>

#include "client_common.h"

int tcp_connect(struct ct_state *state, json_t *hai_list)
{
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int sfd, s;
	int rc;

again:
	if (state->socket_fd != -1) {
		close(state->socket_fd);
		state->socket_fd = -1;
	}
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	s = getaddrinfo(state->config.host, state->config.port, &hints,
			&result);
	if (s != 0) {
		if (s == EAI_AGAIN)
			goto again;
		if (s == EAI_SYSTEM) {
			rc = -errno;
			LOG_ERROR(rc, "ERROR getaddrinfo for %s:%s",
				  state->config.host, state->config.port);
		} else {
			rc = -EIO;
			LOG_ERROR(rc, "ERROR getaddrinfo for %s:%s: %s",
				  state->config.host, state->config.port,
				  gai_strerror(s));
		}
		return rc;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sfd == -1)
			continue;

		if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0)
			break; /* Success */

		close(sfd);
	}
	freeaddrinfo(result);

	if (rp == NULL) {
		LOG_WARN(-errno, "Could not connect to %s:%s. Retrying.",
			 state->config.host, state->config.port);
		sleep(5);
		goto again;
	}
	LOG_INFO("Connected to %s", state->config.host);

	state->socket_fd = sfd;

	rc = protocol_request_ehlo(state, hai_list);
	if (rc) {
		LOG_WARN(
			rc,
			"Just connected but could not send request? reconnecting");
		sleep(5);
		goto again;
	}
	rc = protocol_read_command(state->socket_fd, "server", NULL,
				   protocol_ehlo_cbs, state);
	if (rc) {
		LOG_WARN(
			rc,
			"Just connected but did not get correct ehlo? reconnecting");
		sleep(5);
		goto again;
	}

	return 0;
}
