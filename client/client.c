/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <stdint.h>

#include "client.h"
#include "lustre.h"
#include "version.h"

void print_help(char *argv[]) {
	printf("Usage: %s [options]\n\n", argv[0]);
	printf("common client options are shared with lib (see config file and env var\n");
	printf("defaults to printing status\n\n");
	printf("options:\n");
	printf("--host/-H: server to connect to\n");
	printf("--port/-p: port to connect to\n");
	printf("--queue/-Q: queue active_requests from stdin\n");
	printf("--recv/-R: (debug tool) ask for receiving work\n");
	printf("           note the work will be reclaimed when client disconnects\n");
	printf("--iters/-i: number of replies to expect (can be used to wait after\n");
	printf("            receiving work, negative number loops forever)\n");
	printf("--verbose/-v: Increase log level (can repeat)\n");
	printf("--quiet/-q: Decreate log level\n");
	printf("--version/-V: show version\n");
	printf("--help/-h: This help\n");
}

void print_version(void) {
	printf("Coordinatool client version %s\n", VERSION);
}

int parse_hai_cb(struct hsm_action_item *hai, unsigned int archive_id,
		 unsigned long flags, void *arg) {
	struct active_requests_state *active_requests = arg;
	json_t *json_hai = json_hsm_action_item(hai, archive_id, flags);

	if (!json_hai) {
		LOG_ERROR(-errno, "Could not pack hai to json");
		return -1;
	}

	json_array_append_new(active_requests->hai_list, json_hai);

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

	rc = tcp_connect(state, NULL);
	if (rc < 0)
		return rc;

	switch (client->mode) {
	case MODE_STATUS:
		protocol_request_status(state);
		break;
	case MODE_QUEUE:
		client->active_requests.hai_list = json_array();
		if (!client->active_requests.hai_list)
			abort();
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
		protocol_request_queue(state,
				       client->active_requests.hai_list);
		break;
	case MODE_RECV:
		protocol_request_recv(state);
		break;
	default:
		LOG_ERROR(-EINVAL, "Unkonwn mode %d", client->mode);
		return -EINVAL;
	}

	while (client->iters < 0 || client->iters-- > 0) {
		protocol_read_command(state->socket_fd, "server", NULL,
				      protocol_cbs, state);
	}
	return 0;
}

#define OPT_FSNAME 257

int main(int argc, char *argv[]) {
	struct option long_opts[] = {
		{ "verbose", no_argument, NULL, 'v' },
		{ "version", no_argument, NULL, 'V' },
		{ "help", no_argument, NULL, 'h'},
		{ "quiet",   no_argument, NULL, 'q' },
		{ "port", required_argument, NULL, 'p' },
		{ "host", required_argument, NULL, 'H' },
		{ "queue", no_argument, NULL, 'Q' },
		{ "fsname", required_argument, NULL, OPT_FSNAME },
		{ "recv", no_argument, NULL, 'R' },
		{ "iters", required_argument, NULL, 'i' },
		{ 0 },
	};
	int rc;

	// default options
	struct client client = {
		.mode = MODE_STATUS,
		.iters = 1,
		.state.socket_fd = -1,
	};

	/* slight hack: if first arg is --config use it to overwrite
	 * conf file... This is mostly for fuzzing parser
	 */
	if (argc > 2 && !strcmp(argv[1], "--config"))
		client.state.config.confpath = argv[2];

	rc = ct_config_init(&client.state.config);
	if (rc) {
		LOG_ERROR(rc, "Could not init config");
		return EXIT_FAILURE;
	}

	/* we don't want an id for debug client */
	free((void*)client.state.config.client_id);
	client.state.config.client_id = NULL;

	while ((rc = getopt_long(argc, argv, "vqH:p:QRi:Vh",
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
			client.mode = MODE_QUEUE;
			break;
		case 'R':
			client.mode = MODE_RECV;
			break;
		case 'i':
			client.iters = atoi(optarg);
			break;
		case OPT_FSNAME:
			if (client.mode != MODE_QUEUE) {
				LOG_ERROR(-EINVAL, "fsname can only be set after -Q");
				return EXIT_FAILURE;
			}
			client.active_requests.fsname = optarg;
			break;
		case 'V':
			print_version();
			return EXIT_SUCCESS;
		case 'h':
			print_help(argv);
			return EXIT_SUCCESS;
		default:
			return EXIT_FAILURE;
		}
	}
	if (argc != optind) {
		LOG_ERROR(-EINVAL, "extra argument specified");
		return EXIT_FAILURE;
	}

	rc = client_run(&client);
	ct_config_free(&client.state.config);
	if (rc)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
