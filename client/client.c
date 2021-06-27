/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <stdint.h>

#include "client.h"
#include "lustre.h"

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
		protocol_read_command(state->socket_fd, NULL, protocol_cbs, state);
	}
#endif
	return 0;
}

int client_run(struct client *client) {
	int rc;
	struct ct_state *state = &client->state;

	rc = tcp_connect(state);
	if (rc < 0)
		return rc;

	if (client->send_queue) {
		client->active_requests.hai_list = json_array();
		if (!client->active_requests.hai_list)
			abort();
		// XXX fsname
		strcpy(client->active_requests.fsname, "testfs0");
		rc = parse_active_requests(0, parse_hai_cb,
					   &client->active_requests);
		if (rc < 0) {
			json_decref(client->active_requests.hai_list);
			return rc;
		}

		if (json_array_size(client->active_requests.hai_list) == 0) {
			LOG_DEBUG("Nothing to enqueue, exiting");
			json_decref(client->active_requests.hai_list);
			return 0;
		}
		/* takes ownership of hai_list */
		state->fsname = client->active_requests.fsname;
		protocol_request_queue(state, client->active_requests.archive_id,
				       client->active_requests.flags,
				       client->active_requests.hai_list);
		protocol_read_command(state->socket_fd, NULL, protocol_cbs, state);
		return 0;
	}

	protocol_request_status(state);
	protocol_request_recv(state);
	while(true) {
		protocol_read_command(state->socket_fd, NULL, protocol_cbs, state);
	}

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
	struct client client = { 0 };
	rc = ct_config_init(&client.state.config);
	if (rc) {
		LOG_ERROR(rc, "Could not init config");
		return EXIT_FAILURE;
	}

	while ((rc = getopt_long(argc, argv, "vqH:p:Q",
			         long_opts, NULL)) != -1) {
		switch (rc) {
		case 'v':
			client.state.config.verbose++;
			llapi_msg_set_level(client.state.config.verbose);
			break;
		case 'q':
			client.state.config.verbose--;
			llapi_msg_set_level(client.state.config.verbose);
			break;
		case 'H':
			client.state.config.host = optarg;
			break;
		case 'p':
			client.state.config.port = optarg;
			break;
		case 'Q':
			client.send_queue = true;
			break;
		default:
			return EXIT_FAILURE;
		}
	}
	if (argc != optind) {
		LOG_ERROR(-EINVAL, "extra argument specified");
		return EXIT_FAILURE;
	}


	rc = client_run(&client);
	if (rc)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
