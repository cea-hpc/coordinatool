/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>
#include <limits.h>

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

static const char *client_status_to_str(enum client_status status) {
	switch (status) {
	case CLIENT_INIT: return "init";
	case CLIENT_READY: return "ready";
	case CLIENT_DISCONNECTED: return "disconnected";
	case CLIENT_WAITING: return "waiting";
	default: return "INVALID";
	}
}

static int protocol_reply_status_client(json_t *clients, struct cds_list_head *head) {
	struct cds_list_head *n;
	int rc;

	cds_list_for_each(n, head) {
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
		    (rc = protocol_setjson_str(c, "status", client_status_to_str(client->status)))) {
			json_decref(c);
			return rc;
		}
		if (client->status == CLIENT_DISCONNECTED &&
		    (rc == protocol_setjson_int(c, "disconnected_timestamp",
						client->disconnected_timestamp))) {
			json_decref(c);
			return rc;
		}

		json_array_append_new(clients, c);
	}
	return 0;
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

	rc = protocol_reply_status_client(clients, &ct_stats->clients);
	if (rc) {
		goto out_freereply;
	}
	rc = protocol_reply_status_client(clients, &ct_stats->disconnected_clients);
	if (rc) {
		goto out_freereply;
	}

	if ((rc = protocol_setjson(reply, "clients", clients)))
		goto out_freereply;

	if (protocol_write(reply, client->fd, client->id, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "%s (%d): Could not write reply: %s",
			  client->id, client->fd, json_str);
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

#ifdef DEBUG_ACTION_NODE
	CDS_INIT_LIST_HEAD(&client->waiting_node);
#endif
	cds_list_add(&client->waiting_node, &state->waiting_clients);
	client->status = CLIENT_WAITING;
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

	if (client->status != CLIENT_READY) {
		return protocol_reply_recv(client, NULL, 0, 0, NULL, EINVAL,
					   "Client must send EHLO first and not already be in RECV");
	}

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

	if (protocol_write(reply, client->fd, client->id, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "%s (%d): Could not write reply: %s",
			  client->id, client->fd, json_str);
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

	uint64_t cookie;
	struct lu_fid dfid;

	if (json_hsm_action_key_get(json, &cookie, &dfid))
		return protocol_reply_done(client, EINVAL,
					   "cookie or fid not set -- old client?");
	struct hsm_action_node *han =
		hsm_action_search_queue(&state->queues, cookie, &dfid);
	if (!han)
		return protocol_reply_done(client, EINVAL,
					   "Request not found");

	int status = protocol_getjson_int(json, "status", 0);
	LOG_INFO("%s (%d): Finished processing "DFID" (cookie %lx): status %d" ,
		 client->id, client->fd, PFID(&han->info.dfid), cookie,
		 status);

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

	if (client->status == CLIENT_WAITING) {
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
		LOG_ERROR(rc, "%s (%d): Could not write reply: %s",
			  client->id, client->fd, json_str);
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
	int enqueued = 0, skipped = 0;
	int rc, final_rc = 0;

	json_t *json_items = json_object_get(json, "hsm_action_items");
	if (!json_items)
		return protocol_reply_queue(client, 0, 0, EINVAL,
					    "No hsm_action_items set");

	const char *fsname = protocol_getjson_str(json, "fsname", NULL, NULL);
	/* fsname is optional */
	if (fsname && strcmp(fsname, state->fsname)) {
		LOG_WARN(-EINVAL, "%s (%d): client sent queue with bad fsname, expected %s got %s",
			 client->id, client->fd, state->fsname, fsname);
		return protocol_reply_queue(client, 0, 0, EINVAL, "Bad fsname");
	}


	unsigned int count;
	json_t *item;
	int64_t timestamp = gettime_ns();
	json_array_foreach(json_items, count, item) {
		struct hsm_action_node *han;
		rc = hsm_action_enqueue_json(state, item, timestamp, &han, client->id);
		if (rc < 0) {
			final_rc = rc;
			continue;
		}
		if (rc > 0) {
			enqueued++;
			LOG_INFO("Enqueued "DFID" (cookie %lx) (from queue request)" ,
				 PFID(&han->info.dfid), han->info.cookie);
		} else {
			skipped++;
		}
	}
	if (final_rc)
		return protocol_reply_queue(client, enqueued, skipped, final_rc,
					    "Error while parsing hsm action items");

	return protocol_reply_queue(client, enqueued, skipped, 0, NULL);
}

int protocol_reply_queue(struct client *client, int enqueued, int skipped,
			 int status, char *error) {
	json_t *reply;
	int rc;

	reply = json_object();
	if (!reply)
		abort();
	if ((rc = protocol_setjson_str(reply, "command", "queue")) ||
	    (rc = protocol_setjson_int(reply, "status", status)) ||
	    (rc = protocol_setjson_str(reply, "error", error)) ||
	    (rc = protocol_setjson_int(reply, "enqueued", enqueued)) ||
	    (rc = protocol_setjson_int(reply, "skipped", skipped)))
		goto out_freereply;

	if (protocol_write(reply, client->fd, client->id, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "%s (%d): Could not write reply: %s",
			  client->id, client->fd, json_str);
		free(json_str);
		goto out_freereply;
	};

out_freereply:
	json_decref(reply);
	return rc;
}

static bool ehlo_is_id_unique(struct state *state, const char *id) {
	struct cds_list_head *n;
	cds_list_for_each(n, &state->stats.clients) {
		struct client *client =
			caa_container_of(n, struct client, node_clients);

		// skip initializing clients
		// note disconnected clients are not in this list
		if (client->status == CLIENT_INIT) {
			continue;
		}

		if (!strcmp(id, client->id)) {
			return false;
		}
	}
	return true;
}

static int ehlo_cb(void *fd_arg, json_t *json, void *arg) {
	struct client *client = fd_arg;
	struct state *state = arg;
	const char *id;
	json_t *json_archives;

	if (client->status != CLIENT_INIT) {
		return protocol_reply_ehlo(client, EINVAL,
					   "Client cannot send EHLO twice");
	}

	json_archives = json_object_get(json, "archive_ids");
	if (json_archives) {
		size_t len, i;
		json_t *json_id;

		len = json_array_size(json_archives);
		free(client->archives);
		client->archives = xmalloc((len + 1) * sizeof(int));
		json_array_foreach(json_archives, i, json_id) {
			json_int_t id = json_integer_value(json_id);
			// non-integers return 0, and 0 is not a valid id
			if (id <= 0 || id > INT_MAX) {
				LOG_ERROR(-EINVAL, "%s (%d): Client sent invalid archive id: %lld",
					  client->id, client->fd, id);
				return protocol_reply_ehlo(client, EINVAL,
							   "Bad archive id in list");
			}
			client->archives[i] = id;
		}
		assert(i == len);
		client->archives[len] = 0;
	}

	id = protocol_getjson_str(json, "id", NULL, NULL);
	if (! ehlo_is_id_unique(state, id ? id : client->id)) {
		LOG_INFO("Clients: duplicate id '%s' refused for %s (%d)" ,
			 id ? id : client->id, client->id, client->fd);
		return protocol_reply_ehlo(client, EEXIST,
					   "id already used by another client");
	}
	client->status = CLIENT_READY;
	if (!id) {
		// no id: no special treatment
		return protocol_reply_ehlo(client, 0, NULL);
	}

	LOG_INFO("Clients: '%s' renamed to %s (%d)", client->id, id, client->fd);
	free((void*)client->id);
	client->id = xstrdup(id);
	client->id_set = true;


	struct cds_list_head *n, *nnext;
	cds_list_for_each(n, &state->stats.disconnected_clients) {
		struct client *old_client =
			caa_container_of(n, struct client, node_clients);

		if (strcmp(id, old_client->id))
			continue;

		LOG_INFO("Clients: restoring state from previously disconnected client %s (%d)",
			 id, client->fd);
		/* move all requests to new client: splice then update pointers in han */
		struct cds_list_head *old_lists[] = {
			&old_client->active_requests,
			&old_client->queues.waiting_restore,
			&old_client->queues.waiting_archive,
			&old_client->queues.waiting_remove,
		};
		struct cds_list_head *new_lists[] = {
			&client->active_requests,
			&client->queues.waiting_restore,
			&client->queues.waiting_archive,
			&client->queues.waiting_remove,
		};
		static_assert(sizeof(old_lists) == sizeof(new_lists));
		for (unsigned int i = 0; i < countof(old_lists); i++) {
			cds_list_splice(old_lists[i], new_lists[i]);
			CDS_INIT_LIST_HEAD(old_lists[i]);
			struct cds_list_head *han_node;
			cds_list_for_each(han_node, new_lists[i]) {
				struct hsm_action_node *han =
					caa_container_of(han_node, struct hsm_action_node, node);
				han->queues = &client->queues;
				// shouldn't be needed, but better safe...
				han->client = NULL;
			}
		}

		// we no longer need it, free it immediately (unset id_set to lower debug message)
		// note we cannot free it right here as queued entries
		old_client->id_set = false;
		client_free(old_client);

		// there can only be one
		break;
	}


	struct cds_list_head free_hai;
	CDS_INIT_LIST_HEAD(&free_hai);
	cds_list_splice(&client->active_requests, &free_hai);
	CDS_INIT_LIST_HEAD(&client->active_requests);


	json_t *hai_list = json_object_get(json, "hai_list");
	int64_t timestamp = gettime_ns();
	if (hai_list) {
		unsigned int count;
		json_t *hai;
		json_array_foreach(hai_list, count, hai) {
			uint64_t cookie;
			struct lu_fid dfid;
			if (json_hsm_action_key_get(hai, &cookie, &dfid)) {
				LOG_WARN(-EINVAL, "%s (%d): No cookie or dfid set for entry in ehlo, version mismatch?",
					 client->id, client->fd);
				continue;
			}

			/* Look for existing action node first: if found this will
			 * automatically remove it from free_hai list.
			 */
			struct hsm_action_node *han;
			han = hsm_action_search_queue(&state->queues, cookie, &dfid);
			if (han) {
#ifdef DEBUG_ACTION_NODE
				LOG_DEBUG("%s (%d): Moving han %p to active requests %p (ehlo)",
					  client->id, client->fd,
					  (void*)han, (void*)&client->active_requests);
#endif
				cds_list_del(&han->node);
				cds_list_add_tail(&han->node, &client->active_requests);
				continue;
			}
			/* Otherwise create new one. Use enqueue to enrich it just in case.
			 * Note that requires a dequeue to immediately get it off waiting list */
			if (hsm_action_enqueue_json(state, hai, timestamp, &han, client->id) < 0 || !han) {
				/* ignore bad items in hai list */
				continue;
			}
#ifdef DEBUG_ACTION_NODE
			LOG_DEBUG("%s (%d): Moving new han %p to active requests %p (ehlo)",
				  client->id, client->fd,
				  (void*)han, (void*)&client->active_requests);
#endif
			hsm_action_dequeue(&state->queues, han);
			cds_list_add_tail(&han->node, &client->active_requests);
			han->client = client;
		}
	}

	/* requeue anything left */
	cds_list_for_each_safe(n, nnext, &free_hai) {
                struct hsm_action_node *node =
			caa_container_of(n, struct hsm_action_node, node);
		cds_list_del(n);
		hsm_action_requeue(node, true);
		/* XXX stats running_restore/archive/remove not accounted properly
		 * fix client_free as well
		 */
	}

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
		LOG_ERROR(rc, "%s (%d): Could not write reply: %s",
			  client->id, client->fd, json_str);
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
