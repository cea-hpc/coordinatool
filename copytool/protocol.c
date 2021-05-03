/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"

/* stop enqueuing new hasm action items if we cannot enqueue at least
 * HAI_SIZE_MARGIN more.
 * That is because item is variable size depending on its data.
 * This is mere optimisation, if element didn't fit it is just put back
 * in waiting list -- at the end, so needs avoiding in general.
 */
#define HAI_SIZE_MARGIN (sizeof(struct hsm_action_item) + 100)

/**************
 *  callbacks *
 **************/

static int status_cb(int fd, json_t *json UNUSED, void *arg) {
	struct state *state = arg;

	return protocol_reply_status(fd, &state->stats, 0, NULL);
}

int protocol_reply_status(int fd, struct ct_stats *ct_stats, int status,
			  char *error) {
	json_t *reply;
	int rc;

	reply = json_object();
	if (!reply)
		abort();
	if ((rc = protocol_setjson_str(reply, "command", "status")) ||
	    (rc = protocol_setjson_int(reply, "status", status)) ||
	    (rc = protocol_setjson_str(reply, "error", error)) ||
	    (rc = protocol_setjson_int(reply, "running_archive", ct_stats->running_archive)) ||
	    (rc = protocol_setjson_int(reply, "running_restore", ct_stats->running_restore)) ||
	    (rc = protocol_setjson_int(reply, "running_remove", ct_stats->running_remove)) ||
	    (rc = protocol_setjson_int(reply, "pending_archive", ct_stats->pending_archive)) ||
	    (rc = protocol_setjson_int(reply, "pending_restore", ct_stats->pending_restore)) ||
	    (rc = protocol_setjson_int(reply, "pending_remove", ct_stats->pending_remove)) ||
	    (rc = protocol_setjson_int(reply, "done_archive", ct_stats->done_archive)) ||
	    (rc = protocol_setjson_int(reply, "done_restore", ct_stats->done_restore)) ||
	    (rc = protocol_setjson_int(reply, "done_remove", ct_stats->done_remove)) ||
	    (rc = protocol_setjson_int(reply, "clients_connected", ct_stats->clients_connected)))
		goto out_freereply;

	if (protocol_write(reply, fd, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "Could not write reply to %d: %s", fd, json_str);
		free(json_str);
		goto out_freereply;
	};

out_freereply:
	json_decref(reply);
	return rc;
}


static int recv_cb(int fd, json_t *json, void *arg) {
	struct state *state = arg;
	struct hsm_action_item *hai;
	size_t bytes_left = protocol_getjson_int(json, "max_bytes", 1024*1024);
	int restore_left = protocol_getjson_int(json, "max_restore", -1);
	int archive_left = protocol_getjson_int(json, "max_archive", -1);
	int remove_left = protocol_getjson_int(json, "max_remove", -1);
	int enqueued_items = 0;
	json_t *hai_list = json_array();
	if (!hai_list)
		abort();

	/* check if there are pending requests
	 * priority restore > remove > archive is hardcoded for now */
	enum hsm_copytool_action actions[] = {
		HSMA_RESTORE, HSMA_REMOVE, HSMA_ARCHIVE,
	};
	int left_count[] = { restore_left, remove_left, archive_left };
	for (size_t i = 0; i < sizeof(actions) / sizeof(*actions); i++) {
		while (bytes_left > HAI_SIZE_MARGIN && left_count[i] != 0) {
			hai = hsm_action_dequeue(&state->queues, actions[i]);
			if (!hai)
				break;
			if (bytes_left < sizeof(*hai) + hai->hai_len) {
				/* did not fit, requeue - this also makes a new copy */
				hsm_action_enqueue(&state->queues, hai);
				queue_node_free(hai);
				break;
			}
			json_array_append_new(hai_list, json_hsm_action_item(hai));
			enqueued_items++;
			if (left_count[i] > 0)
				left_count[i]--;
			queue_node_free(hai);
		}
	}

	if (enqueued_items) {
		json_t *hal = json_object();

		/* XXX common fields are assumed to be the same for now */
		if (!hal ||
		    protocol_setjson_int(hal, "hal_version", HAL_VERSION) ||
		    protocol_setjson_int(hal, "hal_count", enqueued_items) ||
		    protocol_setjson_int(hal, "hal_archive_id", state->queues.archive_id) ||
		    protocol_setjson_int(hal, "hal_flags", state->queues.hal_flags) ||
		    protocol_setjson_str(hal, "hal_fsname", state->queues.fsname) ||
		    protocol_setjson(hal, "list", hai_list))
			abort();
		// frees hal
		return protocol_reply_recv(fd, hal, 0, NULL);
	}
	/* register as waiting client */
	return protocol_reply_recv(fd, NULL, ENOTSUP, "Wait not implemented yet");
}

int protocol_reply_recv(int fd, json_t *hal, int status, char *error) {
	json_t *reply;
	int rc;

	reply = json_object();
	if (!reply)
		abort();

	if (hal && (rc = protocol_setjson(reply, "hsm_action_list", hal)))
		goto out_freereply;

	if ((rc = protocol_setjson_str(reply, "command", "recv")) ||
	    (rc = protocol_setjson_int(reply, "status", status)) ||
	    (rc = protocol_setjson_str(reply, "error", error)))
		goto out_freereply;

	if (protocol_write(reply, fd, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "Could not write reply to %d: %s", fd, json_str);
		free(json_str);
		goto out_freereply;
	};

out_freereply:
	json_decref(reply);
	return rc;
}


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

	rc = hsm_action_enqueue(enqueue_state->queues, hai);
	if (rc < 0)
		return rc;
	enqueue_state->enqueued += rc;

	return 0;
}

static int queue_cb(int fd, json_t *json, void *arg) {
	struct state *state = arg;
	struct enqueue_state enqueue_state;
	int rc;

	json_t *json_hal = json_object_get(json, "hsm_action_list");
	if (!json_hal)
		return protocol_reply_queue(fd, 0, EINVAL, "No hsm_action_list set");

	enqueue_state.queues =
		hsm_action_queues_get(state,
			protocol_getjson_int(json_hal, "hal_archive_id",
					     ARCHIVE_ID_UNINIT),
			protocol_getjson_int(json_hal, "hal_flags", 0),
			protocol_getjson_str(json_hal, "hal_fsname", NULL, NULL));
	if (!enqueue_state.queues)
		return protocol_reply_queue(fd, 0, EINVAL, "No queue found");
	enqueue_state.enqueued = 0;

	rc = json_hsm_action_list_get(json_hal, &enqueue_state.hal,
		sizeof(enqueue_state) - offsetof(struct enqueue_state, hal),
		queue_cb_enqueue, &enqueue_state);
	if (rc < 0)
		return protocol_reply_queue(fd, enqueue_state.enqueued, rc,
				"Error while parsing hsm action list");

	return protocol_reply_queue(fd, enqueue_state.enqueued, 0, NULL);
}

int protocol_reply_queue(int fd, int enqueued, int status, char *error) {
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

	if (protocol_write(reply, fd, 0) != 0) {
		char *json_str = json_dumps(reply, 0);
		rc = -EIO;
		LOG_ERROR(rc, "Could not write reply to %d: %s", fd, json_str);
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
	[QUEUE] = queue_cb,
};
