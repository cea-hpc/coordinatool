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
	PROTOCOL_COMMANDS_MAX,
};

/**
 * enum to string conversion helpers
 */
enum protocol_commands protocol_str2command(const char *str);
const char *protocol_command2str(enum protocol_commands cmd);


typedef int (*protocol_read_cb)(int fd, json_t *json, void *arg);

/**
 * read json objects and callbacks -- this keeps reading until the end
 * of buffer coincides with the end of a json object for efficiency
 *
 * @param fd fd of socket to read one json object from
 * @param cbs vector of callbacks, must be readable up to PROTOCOL_COMMANDS_MAX.
 * if cb is null for a given command, an error is logged and message is ignored.
 * @return 0 on success, -errno on error.
 */
int protocol_read_command(int fd, protocol_read_cb *cbs, void *cb_arg);

int protocol_write(json_t *json, int fd, size_t flags);

/**
 * - STATUS command
 *   request properties:
 *     command = "status"
 *   reply properties (any omitted integer means 0):
 *     command = "status"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message)
 *     {running,pending}_{archive,restore,remove} = integer (u32)
 *     done_{archive,restore,remove} = integer (u64)
 *     clients_connected = integer (u32)
 *
 * example:
 * CLIENT: { "command": "status" }
 * SERVER: { "command": "status", "pending_archive": 2, "running_restore": 3 }
 */
/**
 * send status request
 *
 * @param fd socket to write on
 * @return 0 on success, -errno on error
 */
int protocol_request_status(int fd);

struct ct_stats {
	unsigned int running_archive;
	unsigned int running_restore;
	unsigned int running_remove;
	unsigned int pending_archive;
	unsigned int pending_restore;
	unsigned int pending_remove;
	long unsigned int done_archive;
	long unsigned int done_restore;
	long unsigned int done_remove;
	unsigned int clients_connected;
};

/**
 * - RECV command
 *   request properties:
 *     command = "recv"
 *     max_{archive,restore,remove} = integer (s32)
 *      ^ maximum number of requests to send at a time for each type and
 *        cummulative, negative means no limit. defaults to -1
 *     max_bytes = integer (u32)
 *      ^ maximum size of items to send when reencoded, defaults to 1MB
 *        (this is due to how llapi_hsm_copytool_recv works with a static
 *         buffer for kuc in lustre code)
 *     archive_id = integer (u32)
 *      ^ defaults to any if unset or 0
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
 *
 *   fid object properties
 *     f_seq = integer (u64)
 *     f_oid = integer (u32)
 *     f_ver = integer (u32)
 */

/**
 * - DONE command
 *   request properties:
 *     command = "done"
 *     cookies = array of integers (u64), cookies from hsm action items
 *   reply properties:
 *     command = "done"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message)
 */

/**
 * - QUEUE command
 *   request properties:
 *     command = "queue"
 *     hsm_action_list = hsm_action_list object
 *   reply properties:
 *     command = "queue"
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
json_t *json_hsm_action_item(struct hsm_action_item *hai);

/**
 * jansson-like function to get hai from json value
 *
 * @param json input representing hsm_action_item
 * @param hai output hsm_action_item, must have been preallocated
 * @param hai_len size of hai we can write on
 *
 * @return 0 on success, -1 if json isn't correct
 */
int json_hsm_action_item_get(json_t *json, struct hsm_action_item *hai,
			     size_t hai_len);

typedef int (*hal_get_cb)(struct hsm_action_list *hal,
			  struct hsm_action_item *hai, void *arg);
/**
 * helper to parse hsm_action_list json value
 *
 * @param json input json representing a fid
 * @param hal buffer to write to
 * @param hal_len length of the buffer we can write in
 * @param cb optional callback to call on each item
 *           if given, the first item of the list is repeatedly
 *           written each time as it is assumed list will not be
 *           required again later
 * @param cb_arg passed as is to cb if used
 * @return positive number of items processed on success,
 *         -1 on json parsing error, or
 *         -E2BIG if we could not write everything to the hsm action list
 */
int json_hsm_action_list_get(json_t *json, struct hsm_action_list *hal,
			     size_t hal_len, hal_get_cb cb, void *cb_arg);

#endif
