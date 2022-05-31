/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"

/**************
 *  callbacks *
 **************/


/**
 * STATUS
 */

static int status_cb(void *fd_arg, json_t *json UNUSED, void *arg) {
	struct client *client = fd_arg;
	struct state *state = arg;

	return protocol_reply_status(client, &state->stats, 0, NULL);
}

int protocol_reply_status(struct client *client, struct ct_stats *ct_stats,
			  int status, char *error) {
	json_t *reply, *clients = NULL;
	int rc;

	reply = json_object();
	if (!reply)
		abort();
	if ((rc = protocol_setjson_str(reply, "command", "status")) ||
	    (rc = protocol_setjson_int(reply, "status", status)) ||
	    (rc = protocol_setjson_str(reply, "error", error)) ||
	    (rc = protocol_setjson_int(reply, "running_restore", ct_stats->running_restore)) ||
	    (rc = protocol_setjson_int(reply, "running_archive", ct_stats->running_archive)) ||
	    (rc = protocol_setjson_int(reply, "running_remove", ct_stats->running_remove)) ||
	    (rc = protocol_setjson_int(reply, "pending_restore", ct_stats->pending_restore)) ||
	    (rc = protocol_setjson_int(reply, "pending_archive", ct_stats->pending_archive)) ||
	    (rc = protocol_setjson_int(reply, "pending_remove", ct_stats->pending_remove)) ||
	    (rc = protocol_setjson_int(reply, "done_restore", ct_stats->done_restore)) ||
	    (rc = protocol_setjson_int(reply, "done_archive", ct_stats->done_archive)) ||
	    (rc = protocol_setjson_int(reply, "done_remove", ct_stats->done_remove)) ||
	    (rc = protocol_setjson_int(reply, "clients_connected", ct_stats->clients_connected)))
		goto out_freereply;

	clients = json_array();
	if (!clients)
		abort();
	struct cds_list_head *n;
	cds_list_for_each(n, &ct_stats->clients) {
		struct client *client =
			caa_container_of(n, struct client, node_clients);
		json_t *c = json_object();
		if (!c)
			abort();
		if ((rc = protocol_setjson_str(c, "client_id", client->id)) ||
		    (rc = protocol_setjson_int(c, "current_restore", client->current_restore)) ||
		    (rc = protocol_setjson_int(c, "current_archive", client->current_archive)) ||
		    (rc = protocol_setjson_int(c, "current_remove", client->current_remove)) ||
		    (rc = protocol_setjson_int(c, "done_restore", client->done_restore)) ||
		    (rc = protocol_setjson_int(c, "done_archive", client->done_archive)) ||
		    (rc = protocol_setjson_int(c, "done_remove", client->done_remove)) ||
		    (rc = protocol_setjson_int(c, "state", client->state))) { /* XXX convert state to string */
			json_decref(c);
			goto out_freereply;
		}
		json_array_append_new(clients, c);
	}

	if ((rc = protocol_setjson(reply, "clients", clients)))
		goto out_freereply;

	if (protocol_write(reply, client->fd, client->id, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "Could not write reply to %s: %s",
			  client->id, json_str);
		free(json_str);
		goto out_freereply;
	};

out_freereply:
	json_decref(clients);
	json_decref(reply);
	return rc;
}


/**
 * RECV
 */

static int recv_cb(void *fd_arg, json_t *json, void *arg) {
	struct client *client = fd_arg;
	struct state *state = arg;
	client->max_bytes = protocol_getjson_int(json, "max_bytes", 1024*1024);
	client->max_restore = protocol_getjson_int(json, "max_restore", -1);
	client->max_archive = protocol_getjson_int(json, "max_archive", -1);
	client->max_remove = protocol_getjson_int(json, "max_remove", -1);

	if (client->max_bytes < HAI_SIZE_MARGIN)
		return protocol_reply_recv(client, NULL, NULL, EINVAL,
					   "Buffer too small");

	cds_list_add(&client->node_waiting, &state->waiting_clients);
	client->state = CLIENT_WAITING;
	/* schedule immediately in case work is available */
	ct_schedule_client(state, client);
	return 0;
}

int protocol_reply_recv(struct client *client, struct hsm_action_queues *queues,
			json_t *hai_list, int status, char *error) {
	json_t *reply;
	int rc;

	reply = json_object();
	if (!reply)
		abort();

	if (hai_list) {
		if (!queues)
			abort();

		json_t *hal = json_object();

		/* XXX common fields are assumed to be the same for now */
		if (!hal ||
		    protocol_setjson_int(hal, "hal_version", HAL_VERSION) ||
		    protocol_setjson_int(hal, "hal_archive_id", queues->archive_id) ||
		    protocol_setjson_int(hal, "hal_flags", queues->hal_flags) ||
		    protocol_setjson_str(hal, "hal_fsname", queues->fsname) ||
		    protocol_setjson(hal, "list", hai_list))
			abort();

		if (hal && (rc = protocol_setjson(reply, "hsm_action_list", hal)))
			goto out_freereply;
	}

	if ((rc = protocol_setjson_str(reply, "command", "recv")) ||
	    (rc = protocol_setjson_int(reply, "status", status)) ||
	    (rc = protocol_setjson_str(reply, "error", error)))
		goto out_freereply;

	if (protocol_write(reply, client->fd, client->id, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "Could not write reply to %s: %s",
			  client->id, json_str);
		free(json_str);
		goto out_freereply;
	};

out_freereply:
	json_decref(reply);
	return rc;
}


/**
 * DONE
 */
static int done_cb(void *fd_arg, json_t *json, void *arg) {
	struct client *client = fd_arg;
	struct state *state = arg;

	uint64_t cookie = protocol_getjson_int(json, "cookie", 0);
	if (!cookie)
		return protocol_reply_done(client, EINVAL,
					   "Cookie not set?");
	unsigned int archive_id =
		protocol_getjson_int(json, "archive_id", ARCHIVE_ID_UNINIT);
	if (archive_id == ARCHIVE_ID_UNINIT)
		return protocol_reply_done(client, EINVAL,
					   "No or invalid archive_id");
	struct hsm_action_queues *queues =
		hsm_action_queues_get(state, archive_id, 0, NULL);
	if (!queues)
		return protocol_reply_done(client, EINVAL,
					   "Do not know archive id");

	struct hsm_action_node *han =
		hsm_action_search_queue(queues, cookie, false);
	if (!han)
		return protocol_reply_done(client, EINVAL,
					   "Unknown cookie sent");

	int status = protocol_getjson_int(json, "status", 0);
	LOG_INFO("%d processed "DFID": %d" ,
		  client->fd, PFID(&han->info.dfid), status);

	cds_list_del(&han->node);
	int action = han->info.action;
	queue_node_free(han);

	/* adjust running action count */
	switch (action) {
	case HSMA_RESTORE:
		client->current_restore--;
		client->done_restore++;
		state->stats.running_restore--;
		state->stats.done_restore++;
		break;
	case HSMA_ARCHIVE:
		client->current_archive--;
		client->done_archive++;
		state->stats.running_archive--;
		state->stats.done_archive++;
		break;
	case HSMA_REMOVE:
		client->current_remove--;
		client->done_remove++;
		state->stats.running_remove--;
		state->stats.done_remove++;
		break;
	default:
		return -EINVAL;
	}

	if (client->state == CLIENT_WAITING) {
		ct_schedule_client(state, client);
	}

	return protocol_reply_done(client, 0, NULL);
}

int protocol_reply_done(struct client *client, int status, char *error) {
	json_t *reply;
	int rc;

	reply = json_object();
	if (!reply)
		abort();
	if ((rc = protocol_setjson_str(reply, "command", "done")) ||
	    (rc = protocol_setjson_int(reply, "status", status)) ||
	    (rc = protocol_setjson_str(reply, "error", error)))
		goto out_freereply;

	if (protocol_write(reply, client->fd, client->id, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "Could not write reply to %s: %s",
			  client->id, json_str);
		free(json_str);
		goto out_freereply;
	};

out_freereply:
	json_decref(reply);
	return rc;
}


/**
 * QUEUE
 */

struct enqueue_state {
	int enqueued;
	struct hsm_action_queues *queues;
	struct hsm_action_list hal;
	char hal_padding[1024];
};

static int queue_cb_enqueue(struct hsm_action_list *hal UNUSED,
			    struct hsm_action_item *hai, void *arg) {
	struct enqueue_state *enqueue_state = arg;
	int rc;

	// XXX get correct queues from hal
	rc = hsm_action_enqueue(enqueue_state->queues, hai);
	if (rc < 0)
		return rc;
	if (rc > 0)
		LOG_INFO("Enqueued a request for "DFID" from QUEUE" ,
			 PFID(&hai->hai_dfid));
	enqueue_state->enqueued += rc;

	return 0;
}

static int queue_cb(void *fd_arg, json_t *json, void *arg) {
	struct client *client = fd_arg;
	struct state *state = arg;
	struct enqueue_state enqueue_state;
	int rc;

	json_t *json_hal = json_object_get(json, "hsm_action_list");
	if (!json_hal)
		return protocol_reply_queue(client, 0, EINVAL,
					    "No hsm_action_list set");

	enqueue_state.queues =
		hsm_action_queues_get(state,
			protocol_getjson_int(json_hal, "hal_archive_id", 0),
			protocol_getjson_int(json_hal, "hal_flags", 0),
			protocol_getjson_str(json_hal, "hal_fsname", NULL, NULL));
	if (!enqueue_state.queues)
		return protocol_reply_queue(client, 0, EINVAL,
					    "No queue found");
	enqueue_state.enqueued = 0;

	// XXX we don't actually need to convert to hai here, just extract required infos
	rc = json_hsm_action_list_get(json_hal, &enqueue_state.hal,
		sizeof(enqueue_state) - offsetof(struct enqueue_state, hal),
		queue_cb_enqueue, &enqueue_state);
	if (rc < 0)
		return protocol_reply_queue(client,
				enqueue_state.enqueued, rc,
				"Error while parsing hsm action list");

	return protocol_reply_queue(client, enqueue_state.enqueued, 0, NULL);
}

int protocol_reply_queue(struct client *client, int enqueued,
			 int status, char *error) {
	json_t *reply;
	int rc;

	reply = json_object();
	if (!reply)
		abort();
	if ((rc = protocol_setjson_str(reply, "command", "queue")) ||
	    (rc = protocol_setjson_int(reply, "status", status)) ||
	    (rc = protocol_setjson_str(reply, "error", error)) ||
	    (rc = protocol_setjson_int(reply, "enqueued", enqueued)))
		goto out_freereply;

	if (protocol_write(reply, client->fd, client->id, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "Could not write reply to %s: %s",
			  client->id, json_str);
		free(json_str);
		goto out_freereply;
	};

out_freereply:
	json_decref(reply);
	return rc;
}

static int ehlo_cb(void *fd_arg, json_t *json, void *arg) {
	struct client *client = fd_arg;
	struct state *state = arg;
	const char *id;

	id = protocol_getjson_str(json, "id", NULL, NULL);
	if (!id) {
		// no id: no special treatment
		return protocol_reply_ehlo(client, 0, NULL);
	}
	free((void*)client->id);
	client->id = xstrdup(id);

	if (!protocol_getjson_bool(json, "reconnect")) {
		// new client, ok. remember id for logs?
		return protocol_reply_ehlo(client, 0, NULL);
	}
	// reconnecting client
	// XXX check filesystem for runnning xfers
	(void) state;
	return protocol_reply_ehlo(client, 0, NULL);
}

int protocol_reply_ehlo(struct client *client, int status, char *error) {
	json_t *reply;
	int rc;

	reply = json_object();
	if (!reply)
		abort();
	if ((rc = protocol_setjson_str(reply, "command", "ehlo")) ||
	    (rc = protocol_setjson_int(reply, "status", status)) ||
	    (rc = protocol_setjson_str(reply, "error", error)))
		goto out_freereply;

	if (protocol_write(reply, client->fd, client->id, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "Could not write reply to %s: %s",
			  client->id, json_str);
		free(json_str);
		goto out_freereply;
	};

out_freereply:
	json_decref(reply);
	return rc;
}


protocol_read_cb protocol_cbs[PROTOCOL_COMMANDS_MAX] = {
	[STATUS] = status_cb,
	[RECV] = recv_cb,
	[DONE] = done_cb,
	[QUEUE] = queue_cb,
	[EHLO] = ehlo_cb,
};
