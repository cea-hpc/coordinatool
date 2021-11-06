/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include "preload.h"

/* create specified directory.
 * path is not constant because we will modify it in place during run,
 * but final value should be restored
 * Go back one level at a time, check if it exists and recurse if not,
 * then create current level.
 */
static int create_dir(char *path) {
	char *slash;
	int rc;

	slash = strrchr(path, '/');
	if (!slash) {
		LOG_ERROR(-EINVAL, "No / in path %s ?", path);
		return -EINVAL;
	}
	*slash = '\0';
	if (access(path, F_OK)) {
		rc = create_dir(path);
		*slash = '/';
		if (rc)
			return rc;
	}
	*slash = '/';
	if (mkdir(path, 0700)) {
		rc = -errno;
		LOG_ERROR(rc, "mkdir '%s' failed", path);
		return rc;
	}

	return 0;
}

/* open client dir and clean any preexisting transfer:
 * assume any preexisting transfer was killed by the service manager,
 * so run cleanup as well.
 */
int state_init(struct hsm_copytool_private *ct) {
	char state_path[PATH_MAX];
	int rc;

	if (!ct->mnt || !ct->state.config.state_dir_prefix
	    || !ct->state.config.client_id)
		return -EINVAL;

	rc = snprintf(state_path, sizeof(state_path),
			"%s/%s/%s", ct->mnt,
			ct->state.config.state_dir_prefix,
			ct->state.config.client_id);
	if (rc >= (int)sizeof(state_path)) {
		rc = -ERANGE;
		LOG_ERROR(rc, "Could not fit %s/%s/%s in state path",
			  ct->mnt, ct->state.config.state_dir_prefix,
			  ct->state.config.client_id);
		return rc;
	}

	/* optimistically hope dir exists then create it... */
	if (access(state_path, F_OK)) {
		rc = create_dir(state_path);
		if (rc)
			return rc;
	}

	ct->client_dir = opendir(state_path);
	if (!ct->client_dir) {
		rc = -errno;
		LOG_ERROR(rc, "Could not open %s", state_path);
		return rc;
	}

	/* cleanup preexisting files */
	state_cleanup(ct, false);

	return 0;
}

/* cleanup state dir content */
void state_cleanup(struct hsm_copytool_private *ct,
		   bool final) {
	struct dirent *dirent;
	int dir_fd;

	if (!ct->client_dir)
		return;

	dir_fd = dirfd(ct->client_dir);
	assert(dir_fd >= 0);
	/* cleanup any existing state file */
	rewinddir(ct->client_dir);
	while ((dirent = readdir(ct->client_dir))) {
		unlinkat(dir_fd, dirent->d_name, 0);
	}

	if (final) {
		/* remove client dir itself.
		 * XXX make it an option to leave it there? */
		char state_path[PATH_MAX];
		/* assumes it fit because it did earlier, if it didn't we'll just try
		 * to unlink something that doesn't exist. */
		snprintf(state_path, sizeof(state_path),
			 "%s/%s/%s", ct->mnt,
			 ct->state.config.state_dir_prefix,
			 ct->state.config.client_id);
		rmdir(state_path);

		closedir(ct->client_dir);
	}
}

/* create file containing json format hai for reloading
 */
int state_createfile(const struct hsm_copytool_private *ct,
		     uint64_t cookie, json_t *action_item) {
	int dir_fd;
	char cookie_str[22];
	int fd, rc;

	if (!ct->client_dir)
		return -EINVAL;

	dir_fd = dirfd(ct->client_dir);
	assert(dir_fd >= 0);

	/* compiler should check this never overflows... */
	sprintf(cookie_str, "%lx", cookie);
	fd = openat(dir_fd, cookie_str, O_CREAT|O_EXCL|O_WRONLY, 0600);
	if (fd < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not create state file %s", cookie_str);
		return rc;
	}
	rc = json_dumpfd(action_item, fd, 0);
	close(fd);
	if (rc) {
		rc = -EIO; /* jansson does not tell us why IO failed... */
		LOG_ERROR(rc, "Could not write state file %s", cookie_str);
		return rc;
	}

	return 0;
}

void state_removefile(const struct hsm_copytool_private *ct UNUSED,
		      uint64_t cookie UNUSED) {
	int dir_fd;
	char cookie_str[22];

	if (!ct->client_dir)
		return;

	dir_fd = dirfd(ct->client_dir);
	assert(dir_fd >= 0);

	/* compiler should check this never overflows... */
	sprintf(cookie_str, "%lx", cookie);
	unlinkat(dir_fd, cookie_str, 0);
}
