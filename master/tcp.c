/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <netdb.h>

#include "master_ct.h"

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
		LOG_DEBUG("ERROR getaddrinfo: %s", gai_strerror(s));
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

char *sockaddr2str(struct sockaddr_storage *addr, socklen_t len) {
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

	rc = epoll_addfd(state->epoll_fd, fd);
	if (rc < 0) {
		LOG_ERROR(rc, "Could not add client %s to epoll", peer_str);
	}

	free(peer_str);
	return rc;
}
