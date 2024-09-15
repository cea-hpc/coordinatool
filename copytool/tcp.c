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
	s = getaddrinfo(state->config.host, state->config.port, &hints, &result);
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
				  state->config.host, state->config.port, gai_strerror(s));
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
	LOG_INFO("Listening on %s:%s", state->config.host, state->config.port);

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

static void client_closefd(struct state *state, struct client *client) {
	if (client->fd >= 0) {
		close(client->fd);
		state->stats.clients_connected--;
		client->fd = -1;
	}
}

void client_free(struct client *client) {
	struct state *state = client->queues.state;
	struct cds_list_head *n, *next;
	struct cds_list_head *lists[] = {
		&client->active_requests,
		&client->queues.waiting_restore,
		&client->queues.waiting_archive,
		&client->queues.waiting_remove,
	};

	if (client->id_set) {
		    LOG_INFO("Clients: freeing %s (%d)", client->id, client->fd);
	} else {
		    LOG_DEBUG("Clients: freeing anonymous %s (%d)", client->id, client->fd);
	}
	client_closefd(state, client);
	cds_list_del(&client->node_clients);
	if (client->status == CLIENT_WAITING)
		cds_list_del(&client->waiting_node);
	// reassign any request that would be lost
	for (unsigned int i = 0; i < countof(lists); i++) {
		cds_list_for_each_safe(n, next, lists[i]) {
			struct hsm_action_node *node =
				caa_container_of(n, struct hsm_action_node, node);
			hsm_action_move(&state->queues, node, true);
		}
	}
	ct_schedule(state);
	free((void*)client->id);
	free((void*)client->archives);
	free(client);
}

void client_disconnect(struct client *client) {
	struct state *state = client->queues.state;

	/* no point in keeping client around if it has no id */
	if (!client->id_set) {
		client_free(client);
		return;
	}

	switch (client->status) {
	case CLIENT_READY:
	case CLIENT_WAITING:
		/* remember disconnected clients for a bit */
		LOG_INFO("Clients: disconnect %s (%d)", client->id, client->fd);
		if (client->status == CLIENT_WAITING)
			cds_list_del(&client->waiting_node);
		client->status = CLIENT_DISCONNECTED;
		client_closefd(state, client);
		client->disconnected_timestamp = gettime_ns();
		cds_list_del(&client->node_clients);
		cds_list_add(&client->node_clients,
			     &state->stats.disconnected_clients);
		timer_rearm(state);
		break;
	default:
		/* clients who never sent ehlo or aren't actually connected
		 * are just freed */
		client_free(client);
		break;
	}
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

	struct client *client = xcalloc(sizeof(*client), 1);

	client->fd = fd;
	client->id = sockaddr2str(&peer_addr, peer_addr_len);
	CDS_INIT_LIST_HEAD(&client->active_requests);
#ifdef DEBUG_ACTION_NODE
	CDS_INIT_LIST_HEAD(&client->node_clients);
#endif
	cds_list_add(&client->node_clients, &state->stats.clients);
	CDS_INIT_LIST_HEAD(&client->queues.waiting_restore);
	CDS_INIT_LIST_HEAD(&client->queues.waiting_archive);
	CDS_INIT_LIST_HEAD(&client->queues.waiting_remove);
	client->status = CLIENT_INIT;
	client->queues.state = state;
	state->stats.clients_connected++;

	LOG_DEBUG("Clients: new connection %s (%d)", client->id, client->fd);

	rc = epoll_addfd(state->epoll_fd, fd, client);
	if (rc < 0) {
		LOG_ERROR(rc, "%s (%d): Could not add client to epoll",
			  client->id, client->fd);
		client_free(client);
	}

	return rc;
}

struct client *client_new_disconnected(struct state *state, const char *id) {
	/* create client in disconnected state for recovery */
	struct client *client = xcalloc(sizeof(*client), 1);

	client->fd = -1;
	client->id = xstrdup(id);
	CDS_INIT_LIST_HEAD(&client->active_requests);
#ifdef DEBUG_ACTION_NODE
	CDS_INIT_LIST_HEAD(&client->node_clients);
#endif
	cds_list_add(&client->node_clients, &state->stats.disconnected_clients);
	CDS_INIT_LIST_HEAD(&client->queues.waiting_restore);
	CDS_INIT_LIST_HEAD(&client->queues.waiting_archive);
	CDS_INIT_LIST_HEAD(&client->queues.waiting_remove);
	client->status = CLIENT_DISCONNECTED;
	client->disconnected_timestamp = gettime_ns();
	client->queues.state = state;

	LOG_INFO("Clients: restore from redis %s", id);

	return client;
}
