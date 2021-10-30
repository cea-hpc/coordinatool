/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <netdb.h>

#include "coordinatool.h"

int tcp_listen(struct state *state) {
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int sfd, s;
	int rc, optval = 1;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

again:
	s = getaddrinfo(state->host, state->port, &hints, &result);
	if (s != 0) {
		if (s == EAI_AGAIN)
			goto again;
		if (s == EAI_SYSTEM) {
			rc = -errno;
			LOG_ERROR(rc, "ERROR getaddrinfo");
		} else {
			rc = -EIO;
			LOG_ERROR(rc, "ERROR getaddrinfo: %s", gai_strerror(s));
		}
		return rc;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype,
				rp->ai_protocol);
		if (sfd == -1)
			continue;

		if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)))
			LOG_ERROR(errno, "setting SO_REUSEADDR failed, continuing anyway");

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
	rc = epoll_addfd(state->epoll_fd, sfd, (void*)(uintptr_t)sfd);
	if (rc < 0) {
		LOG_ERROR(rc, "Could not add listen socket to epoll");
		return rc;
	}
	LOG_INFO("Listening on %s:%s\n", state->host, state->port);

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
	if (asprintf(&addrstring, "%s:%s", host, service) < 0)
		return NULL;

	return addrstring;
}

void free_client(struct state *state, struct client *client) {
	struct cds_list_head *n, *next;
	LOG_INFO("Disconnecting %d\n", client->fd);
	epoll_delfd(state->epoll_fd, client->fd);
	close(client->fd);
	free(client->addr);
	cds_list_del(&client->node_clients);
	cds_list_del(&client->node_waiting);
	state->stats.clients_connected--;
	// reassign any request that would be lost
	cds_list_for_each_safe(n, next, &client->active_requests) {
		struct hsm_action_node *node =
			caa_container_of(n, struct hsm_action_node, node);
		cds_list_del(n);
		hsm_action_requeue(node);
	}
	free(client);
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

	struct client *client = calloc(sizeof(*client), 1);
	if (!client)
		abort();

	client->fd = fd;
	client->addr = sockaddr2str(&peer_addr, peer_addr_len);
	CDS_INIT_LIST_HEAD(&client->active_requests);
	CDS_INIT_LIST_HEAD(&client->node_waiting);
	cds_list_add(&client->node_clients, &state->stats.clients);
	state->stats.clients_connected++;

	LOG_DEBUG("Got client connection from %s", client->addr);

	rc = epoll_addfd(state->epoll_fd, fd, client);
	if (rc < 0) {
		LOG_ERROR(rc, "Could not add client %s to epoll",
			  client->addr);
		free_client(state, client);
	}

	return rc;
}
