/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>

#include "protocol.h"
#include "logs.h"

enum protocol_commands protocol_str2command(const char *str) {
	if (strcmp(str, "status") == 0) {
		return STATUS;
	}
	if (strcmp(str, "recv") == 0) {
		return RECV;
	}
	if (strcmp(str, "done") == 0) {
		return DONE;
	}
	if (strcmp(str, "queue") == 0) {
		return QUEUE;
	}
	LOG_ERROR(-EINVAL, "%s is not a valid command", str);
	return PROTOCOL_COMMANDS_MAX;
}

const char *protocol_command2str(enum protocol_commands command) {
	static char buf[32];

	switch (command) {
	case STATUS: return "status";
	case RECV: return "recv";
	case DONE: return "done";
	case QUEUE: return "queue";
	default:
		LOG_ERROR(-EINVAL, "invalid command: %d\n", command);
		snprintf(buf, sizeof(buf), "%d", command);
		return buf;
	}
}


int protocol_read_command(int fd, protocol_read_cb *cbs, void *cb_arg) {
	json_t *request;
	json_error_t json_error;
	int rc = 0;

	request = json_loadfd(fd, JSON_ALLOW_NUL, &json_error);
	if (!request) {
		// XXX map json_error_code(error) (enum json_error_code) to errno ?
		rc = -EINVAL;
		LOG_ERROR(rc, "Invalid json while reading %d: %s", fd,
			  json_error.text);
		return rc;
	}

	json_t *command_obj = json_object_get(request, "command");
	if (!command_obj) {
		rc = -EINVAL;
		LOG_ERROR(rc, "Received valid json with no command: %s",
			  json_dumps(request, 0));
		goto out_freereq;
	}
	const char *command_str = json_string_value(command_obj);
	if (!command_str) {
		rc = -EINVAL;
		LOG_ERROR(rc, "Command was not a string: %s",
			  json_dumps(command_obj, 0));
		goto out_freereq;
	}
	enum protocol_commands command = protocol_str2command(command_str);
	if (command == PROTOCOL_COMMANDS_MAX) {
		rc = -EINVAL;
		goto out_freereq;
	}

	cbs[command](request, cb_arg);

out_freereq:
	json_decref(request);
	return rc;
}

static inline int protocol_setjson(json_t *obj, const char *key, json_t *val) {
	// nocheck is only about key being valid utf8, we only use constants
	if (json_object_set_new_nocheck(obj, key, val)) {
		LOG_ERROR(-ENOMEM, "Could not assign key %s to object", key);
		return -ENOMEM;
	}
	return 0;
}

static inline int protocol_setjson_str(json_t *obj, const char *key,
				       char *val) {
	// skip if no value
	if (val == NULL || val[0] == '\0')
		return 0;
	json_t *json_val = json_string(val);
	if (!json_val) {
		LOG_ERROR(-ENOMEM, "Could not instanciate string for %s: %s",
			  key, val);
		return -ENOMEM;
	}
	return protocol_setjson(obj, key, json_val);
}
static inline int protocol_setjson_int(json_t *obj, const char *key,
				       json_int_t val) {
	// skip if no value
	if (val == 0)
		return 0;
	json_t *json_val = json_integer(val);
	if (!json_val) {
		LOG_ERROR(-ENOMEM, "Could not instanciate int for %s: %"JSON_INTEGER_FORMAT,
			  key, val);
		return -ENOMEM;
	}
	return protocol_setjson(obj, key, json_val);
}

int protocol_reply_status(int fd, struct ct_stats *ct_stats, int status,
			  char *error) {
	json_t *reply;
	int rc = 0;

	reply = json_object(); // XXX check if can fail
	if (!reply) {
		rc = -ENOMEM;
		LOG_ERROR(rc, "Could not allocate new object");
		return rc;
	}
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

	if (json_dumpfd(reply, fd, 0) != 0) {
		rc = -EIO;
		LOG_ERROR(rc, "Could not write reply to %d: %s", fd,
			  json_dumps(reply, 0));
		goto out_freereply;
	};

out_freereply:
	json_decref(reply);
	return rc;
}
