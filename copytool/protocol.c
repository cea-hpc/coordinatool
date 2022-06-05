/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>

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
		return protocol_reply_recv(client, NULL, 0, 0, NULL, EINVAL,
					   "Buffer too small");

	cds_list_add(&client->node_waiting, &state->waiting_clients);
	client->state = CLIENT_WAITING;
	/* schedule immediately in case work is available */
	ct_schedule_client(state, client);
	return 0;
}

int protocol_reply_recv(struct client *client,
			const char *fsname, uint32_t archive_id,
			uint64_t hal_flags, json_t *hai_list,
			int status, char *error) {
	json_t *reply;
	int rc;

	reply = json_object();
	if (!reply)
		abort();

	if (hai_list) {
		assert(fsname);
		assert(archive_id != 0);

		json_t *hal = json_object();

		/* XXX common fields are assumed to be the same for now */
		if (!hal ||
		    protocol_setjson_int(hal, "hal_version", HAL_VERSION) ||
		    protocol_setjson_int(hal, "hal_archive_id", archive_id) ||
		    protocol_setjson_int(hal, "hal_flags", hal_flags) ||
		    protocol_setjson_str(hal, "hal_fsname", fsname) ||
		    protocol_setjson(hal, "list", hai_list))
			abort();

		if (hal && (rc = protocol_setjson(reply, "hsm_action_list", hal)))
			goto out_freereply;
	}

	if ((rc = protocol_setjson_str(reply, "command", "recv")) ||
	    (rc = protocol_setjson_int(reply, "status", status)) ||
	    (rc = protocol_setjson_str(reply, "error", error)))
		goto out_freereply;

	client->state = CLIENT_CONNECTED;
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
	struct hsm_action_node *han =
		hsm_action_search_queue(&state->queues, cookie);
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


static int queue_cb(void *fd_arg, json_t *json, void *arg) {
	struct client *client = fd_arg;
	struct state *state = arg;
	int enqueued = 0;
	int rc, final_rc = 0;

	json_t *json_hal = json_object_get(json, "hsm_action_list");
	if (!json_hal)
		return protocol_reply_queue(client, 0, EINVAL,
					    "No hsm_action_list set");

	const char *fsname = protocol_getjson_str(json, "fsname", NULL, NULL);
	/* fsname is optional */
	if (fsname && strcmp(fsname, state->fsname)) {
		LOG_WARN(-EINVAL, "client sent queue with bad fsname, expected %s got %s",
			 state->fsname, fsname);
		return protocol_reply_queue(client, 0, EINVAL, "Bad fsname");
	}

	unsigned int count;
	json_t *item;
	int64_t timestamp = gettime_ns();
	json_array_foreach(json_hal, count, item) {
		/* XXX we'll get either something we get back from client to confirm
		 * their running actions on reconnect, in which case we just check
		 * we know about these items, otherwise it's new stuff we need to
		 * enrich
		 */
		rc = hsm_action_enqueue_json(state, item, timestamp);
		if (rc < 0) {
			final_rc = rc;
			continue;
		}

		enqueued++;
	}
	if (final_rc)
		return protocol_reply_queue(client, enqueued, final_rc,
					    "Error while parsing hsm action list");

	return protocol_reply_queue(client, enqueued, 0, NULL);
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

	json_t *hai_list = json_object_get(json, "hai_list");
	if (!hai_list) {
		// new client, ok. remember id for logs?
		return protocol_reply_ehlo(client, 0, NULL);
	}
	// reconnecting client
	// XXX
	// - match if it was a known client
	// - complete running xfer list with what's here if any
	// - remove from running xfer list with what's not here
	// (done while we were out)
	(void) state;
	(void) hai_list;
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
