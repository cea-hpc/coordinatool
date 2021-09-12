/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <sys/socket.h>
#include <netdb.h>

#include "client_common.h"

int tcp_connect(struct ct_state *state) {
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int sfd, s;
	int rc;
	bool reconnect = false;

again:
	if (state->socket_fd != -1) {
		close(state->socket_fd);
		state->socket_fd = -1;
		reconnect = true;
	}
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	s = getaddrinfo(state->config.host, state->config.port, &hints, &result);
	if (s != 0) {
		/* getaddrinfo does not use errno, cheat with debug */
		LOG_ERROR(-EIO, "getaddrinfo: %s", gai_strerror(s));
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
	freeaddrinfo(result);

	if (rp == NULL) {
		LOG_WARN("Could not connect to %s:%s: %d. Retrying.",
			 state->config.host, state->config.port, errno);
		sleep(5);
		goto again;
	}
	LOG_INFO("Connected to %s", state->config.host);

	state->socket_fd = sfd;

	rc = protocol_request_ehlo(state, reconnect);
	if (rc) {
		LOG_WARN("Just connected but could not send request? %d. reconnecting", rc);
		sleep(5);
		goto again;
	}
	rc = protocol_read_command(state->socket_fd, NULL,
				   protocol_ehlo_cbs, state);
	if (rc) {
		LOG_WARN("Just connected but did not get correct ehlo? %d. reconnecting", rc);
		sleep(5);
		goto again;
	}

	return 0;
}
