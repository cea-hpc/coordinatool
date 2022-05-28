/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "coordinatool.h"

static int process_client_state(struct state *state, int state_dir_fd,
				const char *client_name) {
	int rc;

	int client_dir_fd;
	client_dir_fd = openat(state_dir_fd, client_name, O_RDONLY|O_DIRECTORY);
	if (client_dir_fd < 0) {
		LOG_WARN(-errno, "Could not open %s state dir\n", client_name);
				return 0;
	}
	DIR *client_dir;
	client_dir = fdopendir(client_dir_fd);
	assert(client_dir);

	struct client *client;
	rc = create_grace_client(state, client_name, &client);
	if (rc)
		return rc;

	struct dirent *dirent;
	while ((dirent = readdir(client_dir))) {
		if (dirent->d_type != DT_REG)
			continue;
		int action_fd = openat(client_dir_fd, dirent->d_name, O_RDONLY);
		if (action_fd < 0) {
			LOG_WARN(-errno, "Could not open client state file %s/%s",
				 client_name, dirent->d_name);
			continue;
		}
		json_t *json_hai;
		json_error_t json_error;
		json_hai = json_loadfd(action_fd, JSON_ALLOW_NUL, &json_error);
		close(action_fd);
		if (!json_hai){
			LOG_WARN(-EINVAL, "Invalid json while reading state file %s/%s: %s",
				 client_name, dirent->d_name, json_error.text);
			continue;
		}

		unsigned int archive_id;
		uint64_t flags;
		const char *fsname;
		archive_id = protocol_getjson_int(json_hai, "hal_archive_id", 0);
		flags = protocol_getjson_int(json_hai, "hal_flags", 0);
		fsname = protocol_getjson_str(json_hai, "hal_fsname", NULL, NULL);

		struct hsm_action_queues *queues;
		queues = hsm_action_queues_get(state, archive_id, flags, fsname);

		size_t hai_len = 0;
		protocol_getjson_str(json_hai, "hai_data", NULL, &hai_len);

		struct hsm_action_item *hai;
		hai_len = __ALIGN_KERNEL_MASK(sizeof(*hai) + hai_len, 7);
		hai = xmalloc(hai_len);
		rc = json_hsm_action_item_get(json_hai, hai, hai_len);
		if (rc) {
			LOG_WARN(rc, "%s/%s was not valid json for hsm action item?",
				 client_name, dirent->d_name);
			free(hai);
			return rc;
		}
		struct hsm_action_node *han;
		rc = hsm_action_enqueue(queues, hai, &han);
		free(hai);
		if (rc) {
			LOG_WARN(rc, "enqueue failed reading state %s/%s",
				 client_name, dirent->d_name);
			continue;
		}
		han->client = client;
		cds_list_add(&han->node, &client->active_requests);
	}
	return 0;
}

int client_state_init(struct state *state) {
	char state_path[PATH_MAX];
	int rc;
	DIR *state_dir;
	int state_dir_fd;

	/* nothing to do if no state dir set or it doesn't exist */
	if (!state->mntpath || !state->config.state_dir_prefix)
		return 0;
	rc = snprintf(state_path, sizeof(state_path),
		      "%s/%s", state->mntpath, state->config.state_dir_prefix);
	if (rc >= (int)sizeof(state_path)) {
		rc = -ERANGE;
		LOG_ERROR(rc, "Could not fit %s/%s in state_path",
			  state->mntpath, state->config.state_dir_prefix);
		return rc;
	}

	state_dir = opendir(state_path);
	if (!state_dir) {
		if (errno == ENOENT)
			return 0;
		rc = -errno;
		LOG_ERROR(rc, "Could not open %s", state_path);
		return rc;
	}
	rc = 0;
	state_dir_fd = dirfd(state_dir);
	assert(state_dir_fd >= 0);

	struct dirent *dirent;
	while ((dirent = readdir(state_dir))) {
		/* skip non-dir */
		if (dirent->d_type != DT_DIR)
			continue;

		rc = process_client_state(state, state_dir_fd,
					  dirent->d_name);
		if (rc)
			goto out;
	}

out:
	closedir(state_dir);
	return rc;
}
