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
		LOG_ERROR(-EINVAL, "invalid command: %d", command);
		snprintf(buf, sizeof(buf), "%d", command);
		return buf;
	}
}


int protocol_read_command(int fd, protocol_read_cb *cbs, void *cb_arg) {
	json_t *request;
	json_error_t json_error;
	int rc = 0;

	request = json_loadfd(fd, JSON_DISABLE_EOF_CHECK | JSON_ALLOW_NUL,
			      &json_error);
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

	LOG_INFO("Got command %s from %d", command_str, fd);
	if (!cbs || !cbs[command]) {
		rc = -ENOTSUP;
		LOG_ERROR(rc, "command %s not implemented", command_str);
		goto out_freereq;
	}
	cbs[command](fd, request, cb_arg);

out_freereq:
	json_decref(request);
	return rc;
}
