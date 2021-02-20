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

int protocol_read_command(int fd, protocol_read_cb *cbs, void *cb_arg) {
	json_t *obj;
	json_error_t json_error;
	int rc;

	obj = json_loadfd(fd, JSON_ALLOW_NUL, &json_error);
	if (!obj) {
		// XXX map json_error_code(error) (enum json_error_code) to errno ?
		rc = -EINVAL;
		LOG_ERROR(rc, "Invalid json while reading %d: %s", fd,
			  json_error.text);
		return rc;
	}

	json_t *command_obj = json_object_get(obj, "command");
	if (!obj) {
		rc = -EINVAL;
		LOG_ERROR(rc, "Received valid json with no command: %s",
			  json_dumps(obj, 0));
		goto out_freeobj;
	}
	const char *command_str = json_string_value(command_obj);
	if (!command_str) {
		rc = -EINVAL;
		LOG_ERROR(rc, "Command was not a string: %s",
			  json_dumps(obj, 0));
		goto out_freeobj;
	}
	enum protocol_commands command = protocol_str2command(command_str);
	if (command == PROTOCOL_COMMANDS_MAX) {
		rc = -EINVAL;
		goto out_freeobj;
	}

	cbs[command](obj, cb_arg);

out_freeobj:
	json_decref(obj);
	return rc;
}
