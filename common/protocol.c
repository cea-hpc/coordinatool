/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>
#include <errno.h>
#include <sys/param.h>

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


struct load_cb_data {
	int fd;
	char *buffer;
	int buflen;
	int bufoff;
	int bufread;
	int position;
};

// XXX can hang reading from socket on partial json written,
// but I don't see any way around this short of keeping a
// large buffer around
static size_t json_load_cb(void *buffer, size_t buflen, void *data) {
	struct load_cb_data *cbdata = data;

	if (cbdata->bufoff == cbdata->bufread) {
		int n;
		n = read(cbdata->fd, cbdata->buffer, cbdata->buflen);
		if (n < 0) {
			LOG_ERROR(-errno, "Read failed for %d", cbdata->fd);
			return (size_t)-1;
		}
		cbdata->bufread = n;
		cbdata->bufoff = 0;
		if (n == 0)
			return 0;
	}

	size_t len = cbdata->bufread - cbdata->bufoff;
	if (buflen > len)
		buflen = len;
	memcpy(buffer, cbdata->buffer + cbdata->bufoff, buflen);
	cbdata->bufoff += buflen;
	cbdata->position += buflen;

	return buflen;
}

int protocol_read_command(int fd, void *fd_arg, protocol_read_cb *cbs, void *cb_arg) {
	json_t *request;
	json_error_t json_error;
	int rc = 0;
	struct load_cb_data cbdata = {
		.fd = fd,
	};
	cbdata.buffer = malloc(1024*1024);
	cbdata.buflen = 1024*1024;

again:
	request = json_load_callback(json_load_cb, &cbdata,
				     JSON_DISABLE_EOF_CHECK | JSON_ALLOW_NUL,
				     &json_error);
	if (!request) {
		// XXX map json_error_code(error) (enum json_error_code) to errno ?
		rc = -EINVAL;
		/* cbdata.bufread = 0 means eof, readers will close on any error */
		if (cbdata.bufread > 0)
			LOG_ERROR(rc, "Invalid json while reading %d: %s", fd,
				  json_error.text);
		goto out_freebuf;
	}
	/* error's position is the exact offset jansson used, while our's is
	 * what we wrote into jansson. This allows backtracking in buffer if
	 * required
	 */
	if (json_error.position < cbdata.position) {
		assert(cbdata.bufoff >= cbdata.position - json_error.position);
		cbdata.bufoff -= cbdata.position - json_error.position;
	}
	if (llapi_msg_get_level() >= LLAPI_MSG_DEBUG) {
		char *json_str = json_dumps(request, 0);
		LOG_DEBUG("Got something from fd %d: %s\n", fd, json_str);
		free(json_str);
	}

	json_t *command_obj = json_object_get(request, "command");
	if (!command_obj) {
		char *json_str = json_dumps(request, 0);
		rc = -EINVAL;
		LOG_ERROR(rc, "Received valid json with no command: %s", json_str);
		free(json_str);
		goto out_freereq;
	}
	const char *command_str = json_string_value(command_obj);
	if (!command_str) {
		char *json_str = json_dumps(request, 0);
		rc = -EINVAL;
		LOG_ERROR(rc, "Command was not a string: %s", json_str);
		free(json_str);
		goto out_freereq;
	}
	enum protocol_commands command = protocol_str2command(command_str);
	if (command == PROTOCOL_COMMANDS_MAX) {
		rc = -EINVAL;
		goto out_freereq;
	}

	LOG_DEBUG("Got command %s from %d", command_str, fd);
	if (!cbs || !cbs[command]) {
		rc = -ENOTSUP;
		LOG_ERROR(rc, "command %s not implemented", command_str);
		goto out_freereq;
	}
	cbs[command](fd_arg, request, cb_arg);

out_freereq:
	json_decref(request);
	if (rc == 0 && cbdata.bufoff != cbdata.bufread) {
		cbdata.position = 0;
		goto again;
	}

out_freebuf:
	free(cbdata.buffer);
	return rc;
}

static int write_full(int fd, const char *buf, size_t count) {
	ssize_t n;

	while (count > 0) {
		n = write(fd, buf, count);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			LOG_ERROR(-errno, "could not write to %d\n", fd);
			return -1;
		}
		if ((size_t)n > count) {
			LOG_ERROR(-ERANGE, "write returned more than we asked for?!");
			return -1;
		}
		count -= n;
	}
	return 0;
}

static int json_dump_cb(const char *buffer, size_t _size, void *data) {
	struct load_cb_data *cbdata = data;
	int rc;
	ssize_t size = _size;
	
	if (size > cbdata->buflen - cbdata->bufread) {
		rc = write_full(cbdata->fd, cbdata->buffer, cbdata->bufread);
		cbdata->bufread = 0;
		if (rc)
			return rc;
	}
	if (size > cbdata->buflen) {
		// just write directly if it's big, buffer has just been flushed
		rc = write_full(cbdata->fd, buffer, size);
		return rc;
	}
	memcpy(cbdata->buffer + cbdata->bufread, buffer, size);
	cbdata->bufread += size;
	return 0;
}

int protocol_write(json_t *json, int fd, size_t flags) {
	struct load_cb_data cbdata = {
		.fd = fd,
	};
	int rc;

	if (llapi_msg_get_level() >= LLAPI_MSG_DEBUG) {
		char *json_str = json_dumps(json, 0);
		LOG_DEBUG("Sending message to fd %d: %s", fd, json_str);
		free(json_str);
	}

	cbdata.buffer = malloc(64*1024);
	cbdata.buflen = 64*1024;
	rc = json_dump_callback(json, json_dump_cb, &cbdata, flags);
	if (rc == 0 && cbdata.bufread)
		rc = write_full(cbdata.fd, cbdata.buffer, cbdata.bufread);
	free(cbdata.buffer);
	return rc;
}
