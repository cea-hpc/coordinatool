/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef COORDINATOOL_PROTOCOL_H
#define COORDINATOOL_PROTOCOL_H

#include <lustre/lustreapi.h>
#include <jansson.h>

#include "logs.h"

/**
 * Protocol overview:
 * - requests always come from clients (e.g. give up on interrupting
 *   really running requests on hsm cancel), in the form of a single json
 *   object, which then expect a single json object as a response
 * - if a client disconnects, any requests it owned are reassigned to be
 *   redistributed
 * - all the way thorough, a missing key means 0 or null string
 */

enum protocol_commands {
	STATUS,
	RECV,
	DONE,
	QUEUE,
	EHLO,
	PROTOCOL_COMMANDS_MAX,
};

/**
 * enum to string conversion helpers
 */
enum protocol_commands protocol_str2command(const char *str);
const char *protocol_command2str(enum protocol_commands cmd);


typedef int (*protocol_read_cb)(void *fd_arg, json_t *json, void *arg);

/**
 * read json objects and callbacks -- this keeps reading until the end
 * of buffer coincides with the end of a json object for efficiency
 *
 * @param fd fd of socket to read one json object from
 * @param id hint for logs identifying fd
 * @param fd_arg
 * @param cbs vector of callbacks, must be readable up to PROTOCOL_COMMANDS_MAX.
 * if cb is null for a given command, an error is logged and message is ignored.
 * @param cb_arg
 * @return 0 on success, -errno on error.
 */
int protocol_read_command(int fd, const char *id, void *fd_arg,
			  protocol_read_cb *cbs, void *cb_arg);

int protocol_write(json_t *json, int fd, const char *id, size_t flags);

/**
 * - STATUS command: query runtime information
 *   request properties:
 *     command = "status"
 *   reply properties (any omitted integer means 0):
 *     command = "status"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message)
 *     {running,pending}_{archive,restore,remove} = integer (u32)
 *     done_{archive,restore,remove} = integer (u64)
 *     clients_connected = integer (u32)
 *     clients: list of clients with their properties e.g. client_id, status...
 *
 * example:
 * CLIENT: { "command": "status" }
 * SERVER: { "command": "status", "pending_archive": 2, "running_restore": 3 }
 */

/**
 * - RECV command: request work
 *   request properties:
 *     command = "recv"
 *     max_{archive,restore,remove} = integer (s32)
 *      ^ maximum number of requests to send at a time for each type and
 *        cummulative, negative means no limit. defaults to -1
 *     max_bytes = integer (u32)
 *      ^ maximum size of items to send when reencoded, defaults to 1MB
 *        (this is due to how llapi_hsm_copytool_recv works with a static
 *         buffer for kuc in lustre code)
 *   reply properties:
 *     command = "recv"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message)
 *     hsm_action_list = hsm_action_list object
 *
 *   hsm_action_list object properties
 *     hal_version = integer (u32)
 *     hal_count = integer (u32)
 *     hal_archive_id = integer (u32)
 *     hal_flags = integer (u64)
 *     hal_fsname = string
 *     list = array of hsm_action_items objects
 *   note hal_compount_id, hal_flags aren't set (ignored)
 *   hal_flags would require grouping by flags as well and seem unused.
 *
 *   hsm_action_item object properties
 *     hai_action = integer (u32)
 *     hai_fid = fid object
 *     hai_dfid = fid object
 *     hai_extent_offset = integer (u64)
 *     hai_extent_length = integer (u64)
 *     hai_cookie = integer (u64)
 *     hai_gid = integer (u64)
 *     hai_data = string (can contain nul bytes as proper escaped json)
 *   note hai_len is NOT sent, it is computed again from hai_data length and struct size.
 *     extra fields can be sent and are ignored
 *
 *   fid object properties
 *     f_seq = integer (u64)
 *     f_oid = integer (u32)
 *     f_ver = integer (u32)
 */

/**
 * - DONE command: report xfer status
 *   request properties:
 *     command = "done"
 *     archive_id = integer (u32), archive_id of the cookie
 *     hai_cookie = integers (u64), cookie of the hsm action items being acknowledged
 *     hai_dfid = fid object (see recv), dfid of the hsm action item being acknowledged
 *   reply properties:
 *     command = "done"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message)
 */

/**
 * - QUEUE command: add hsm action items to the server
 *   request properties:
 *     command = "queue"
 *     fsname = string (optional, used for sanity check if set)
 *     hsm_action_items = list of 'hsm_action_item' (see RECV), plus:
 *       hal_archive_id = integer (u32)
 *       hal_flags = integer (u64)
 *       timestamp = integer (u64) epoch in ns
 *                   (optional, receive timestamp if unset)
 *
 *   reply properties:
 *     command = "queue"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message)
 *     enqueued = int (number of enqueued requests)
 *     skipped = int (number of skipped (already enqueued) requests)
 */

/**
 * - EHLO command: initial greeting handshake.
 *   Intended for recovery and capabilities negotiation.
 *   request properties:
 *     command = "ehlo"
 *     id = string, optional requested id, should be unique per client
 *          if not set reconnection will not be supported
 *     fsname = string (optional, sanity check we have the right fs)
 *     archive_ids = array of integer (u32) (optional, any archive if unset or empty)
 *     hai_list = list of running hai (see queue)
 *   reply properties:
 *     command = "ehlo"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message)
 */

/**
 * - future command ideas:
 *   * dump (list all started and pending requests, list clients)
 *   * lock/unlock
 *   * change some config value on the fly? could be a single command
 *     that sets lock property above
 */

/**
 * common helpers for packing
 */

static inline int protocol_setjson(json_t *obj, const char *key, json_t *val) {
	// nocheck is only about key being valid utf8, we only use constants
	if (json_object_set_new_nocheck(obj, key, val)) {
		LOG_ERROR(-ENOMEM, "Could not assign key %s to object", key);
		return -ENOMEM;
	}
	return 0;
}

static inline int protocol_setjson_str(json_t *obj, const char *key,
				       const char *val) {
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

static inline int protocol_setjson_bool(json_t *obj, const char *key,
					bool val) {
	// skip if false
	if (!val)
		return 0;
	json_t *json_val = json_true();
	if (!json_val) {
		LOG_ERROR(-ENOMEM, "Could not instanciate true boolean for %s",
			  key);
		return -ENOMEM;
	}
	return protocol_setjson(obj, key, json_val);
}


/**
 * helpers for getting, with default values
 */
static inline json_int_t protocol_getjson_int(json_t *obj, const char *key,
					      json_int_t defval) {
	json_t *json_val = json_object_get(obj, key);
	if (!json_val)
		return defval;

	/* slight premature optimization: jansson will return 0 if not passed
	 * an integer, so try to get the value first and skip the double check
	 * in common case
	 */
	json_int_t val = json_integer_value(json_val);
	if (val == 0 && !json_is_integer(json_val)) {
		LOG_ERROR(-EINVAL, "field %s was set, but not an integer - assuming default",
			  key);
		return defval;
	}
	return val;
}

static inline bool protocol_getjson_bool(json_t *obj, const char *key) {
	json_t *json_val = json_object_get(obj, key);
	if (!json_val)
		return false;

	if (!json_is_boolean(json_val))
		return false;
	return json_is_true(json_val);
}

static inline const char *protocol_getjson_str(json_t *obj, const char *key,
					       const char *defval, size_t *len) {
	json_t *json_val = json_object_get(obj, key);
	if (!json_val)
		goto defval;

	const char *val = json_string_value(json_val);
	if (val == NULL)
		goto defval;

	if (len)
		*len = json_string_length(json_val);
	return val;

defval:
	if (len && defval)
		*len = strlen(defval);
	return defval;
}

/**
 * lustre helpers
 */

/**
 * jansson-like function, fid -> json value
 *
 * @param fid input fid
 * @return json value representing the fid, or NULL on error
 */
json_t *json_fid(struct lu_fid *fid);

/**
 * jansson-like function to get fid from json value
 *
 * @param json input json representing a fid
 * @param fid output fid value
 * @return 0 on success, -1 if json isn't correct (missing, extra field)
 */
int json_fid_get(json_t *json, struct lu_fid *fid);

/**
 * jansson-like function, hai -> json value
 *
 * @param hai input hsm_action_item
 * @return json value representing the fid, or NULL on error
 */
json_t *json_hsm_action_item(struct hsm_action_item *hai,
			     uint32_t archive_id, uint64_t flags);

/**
 * jansson-like function to get hai from json value
 *
 * @param json input representing hsm_action_item
 * @param hai output hsm_action_item, must have been preallocated
 * @param hai_len size of hai we can write on
 * @param pdata pointer to data -- valid while json is valid
 *
 * @return 0 on success, -1 if json isn't correct
 */
int json_hsm_action_item_get(json_t *json, struct hsm_action_item *hai,
			     size_t hai_len, const char **pdata);

/**
 * jansson-like function to get just cookie and dfid from json value
 *
 * @param json input object with hai_cookie and hai_dfid
 * @param hai_cookie output cookie
 * @param hai_dfid output dfid
 *
 * @return 0 on success, -1 if json isn't correct
 */
int json_hsm_action_key_get(json_t *json, uint64_t *hai_cookie,
			     struct lu_fid *hai_dfid);

typedef int (*hal_get_cb)(struct hsm_action_list *hal,
			  struct hsm_action_item *hai,
			  json_t *hai_json, void *arg);
/**
 * helper to parse hsm_action_list json value
 *
 * @param json input json representing a fid
 * @param hal buffer to write to
 * @param hal_len length of the buffer we can write in
 * @param cb optional callback to call on each item
 *           its return value should be:
 *            < 0  error, the function stops immediately
 *            == 0 appends item to hal
 *            == 1 skip hai (overwrites next in same position)
 *            > 1  unspecified
 *           no callback acts as if 0 had been returned (build hal)
 * @param cb_arg passed as is to cb if used
 * @return positive number of items processed on success,
 *         -1 on json parsing error,
 *         -E2BIG if we could not write everything to the hsm action list,
 *         error returned from cb
 */
int json_hsm_action_list_get(json_t *json, struct hsm_action_list *hal,
			     size_t hal_len, hal_get_cb cb, void *cb_arg);

#endif
