/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef MASTER_CT_PROTOCOL_H
#define MASTER_CT_PROTOCOL_H

#include <lustre/lustreapi.h>
#include <jansson.h>

/**
 * Protocol overview:
 * - requests always come from clients (e.g. give up on interrupting
 *   really running requests on hsm cancel), in the form of a single json
 *   object, which then expect a single json object as a response
 * - if a client disconnects, any requests it owned are reassigned to be
 *   redistributed
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


typedef int (*protocol_read_cb)(json_t *json, void *arg);

/**
 * read one json object exactly
 *
 * @param fd fd of socket to read one json object from
 * @param cbs vector of callbacks, must be readable up to PROTOCOL_COMMANDS_MAX.
 * if cb is null for a given command, an error is logged and message is ignored.
 * @return 0 on success, -errno on error.
 */
int protocol_read_command(int fd, protocol_read_cb *cbs, void *cb_arg);

/**
 * - STATUS command
 *   request properties:
 *     command = "status"
 *   reply properties (any omitted integer means 0):
 *     command = "status"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message, optional)
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
 * send status reply
 *
 * @param fd socket to write on
 * @param ct_stats stats to get stats to send from
 * @param status error code
 * @param error nul-terminated error string, can be NULL
 * @return 0 on success, -errno on error
 */
int protocol_reply_status(int fd, struct ct_stats *ct_stats, int status, char *error);

/**
 * - RECV command
 *   request properties:
 *     command = "recv"
 *     max_{archive,restore,remove} = integer
 *      ^ maximum number of requests to send at a time for each type and
 *        cummulative, defaults to 1
 *     max_bytes = integer (u32)
 *      ^ maximum size of items to send when reencoded, defaults to 1MB
 *        (this is due to how llapi_hsm_copytool_recv works with a static 
 *         buffer for kuc in lustre code)
 *     archive_id = integer (u32)
 *      ^ defaults to any if unset or 0
 *   reply properties:
 *     command = "recv"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message, optional)
 *     hsm_action_list = hsm_action_list object
 *
 *   hsm_action_list object properties
 *     hal_version = integer (u32)
 *     hal_count = integer (u32)
 *     hal_archive_id = integer (u32)
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
 *
 * - DONE command
 *   request properties:
 *     command = "done"
 *     cookies = array of integers (u64), cookies from hsm action items
 *   reply properties:
 *     command = "done"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message, optional)
 *
 * - QUEUE command
 *   request properties:
 *     command = "queue"
 *     hsm_action_list = hsm_action_list object
 *   reply properties:
 *     command = "queue"
 *     status = int (0 on success, errno on failure)
 *     error = string (extra error message, optional)
 *
 * - future command ideas:
 *   * dump (list all started and pending requests, list clients)
 *   * lock/unlock
 *   * change some config value on the fly? could be a single command
 *     that sets lock property above
 */

/**
 * helpers
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
 * @param hai output hsm_action_item, must have been preallocated and
 *            hai->hai_len set
 * @return 0 on success, -1 if json isn't correct
 */
int json_hsm_action_item_get(json_t *json, struct hsm_action_item *hai);

/**
 * helper to parse hsm_action_list json value
 *
 * @param json input json representing a fid
 * @param hal buffer to write to
 * @param hal_len length of the buffer we can write in
 * @return 0 on success, -1 on json parsing error, or
 *         -E2BIG if we could not write everything to the hsm action list
 */
int json_hsm_action_list_get(json_t *json, struct hsm_action_list *hal, size_t hal_len);

#endif
