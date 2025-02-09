/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <search.h>

#include "coordinatool.h"

/* dir_fd is guaranteed to be valid in these two functions */
static int reporting_write_to_fs(const char *hint, const char *message)
{
	int rc;
	_cleanup_(closep) int fd = openat(state->reporting_dir_fd, hint,
					  O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (fd < 0) {
		rc = -errno;
		LOG_WARN(rc, "Could not open '%s' in %s", hint,
			 state->config.reporting_dir);
		return rc;
	}

	rc = write_full(fd, message, strlen(message));
	if (rc < 0) {
		LOG_WARN(rc, "Could not write '%s' to '%s/%s'", message,
			 state->config.reporting_dir, hint);
	}

	return rc;
}

static int reporting_unlink(const char *hint)
{
	/* don't unlink on shutdown */
	if (state->terminating)
		return 0;

	return unlinkat(state->reporting_dir_fd, hint, 0);
}

static int reporting_compare(const void *a, const void *b)
{
	const struct reporting *ra = a, *rb = b;

	if (ra->hint_len < rb->hint_len)
		return -1;

	if (ra->hint_len > rb->hint_len)
		return 1;

	return memcmp(ra->hint, rb->hint, ra->hint_len);
}

int report_new_action(struct hsm_action_node *han)
{
	/* disabled in config */
	if (state->reporting_dir_fd < 0)
		return 0;

	const char *hint;
	const char *data = han->info.data;
	const char *hint_needle = state->config.reporting_hint;
	int data_len = han_data_len(han);
	int needle_len = strlen(hint_needle);

	/* can't use strstr on hai data: might contain nul bytes */
	/* note: needle contains the = separator */
	while ((hint = memmem(data, data_len, hint_needle, needle_len))) {
		if (hint == han->info.data || hint[-1] == ',')
			break;
		/* false positive, try again */
		data_len -= hint - data;
		data = hint;
	}
	if (!hint)
		return 0;

	/* strip matched prefix */
	hint += needle_len;
	data_len -= hint - data;

	char *hint_end = memchr(hint, ',', data_len);
	if (hint_end) {
		data_len = hint_end - hint;
	}

	/* filter any unsafe value:
	 * - too long
	 * - not alnum, - or _ (in particular, no /, . or nul byte allowed)
	 */
	if (data_len > 64) {
		LOG_INFO("fid " DFID " reporting hint was longer than 64 (%d)",
			 PFID(&han->info.dfid), data_len);
		return -EINVAL;
	}
	for (const char *c = hint; c < hint + data_len; c++) {
		if (isalnum(*c) || strchr("-_", *c))
			continue;
		LOG_INFO("fid " DFID
			 " reporting hint contained invalid char '%#x'",
			 PFID(&han->info.dfid), *c);
		return -EINVAL;
	}

	struct reporting search = {
		.hint = hint,
		.hint_len = data_len,
	};

	struct reporting **found =
		tfind(&search, &state->reporting_tree, reporting_compare);
	if (!found) {
		struct reporting *new =
			xmalloc(sizeof(struct reporting) + data_len + 1);
		char *new_hint = (char *)(new + 1);
		memcpy(new_hint, hint, data_len);
		new_hint[data_len] = 0;
		new->hint = new_hint;
		new->hint_len = data_len;
		new->refcount = 0;
		found = tsearch(new, &state->reporting_tree, reporting_compare);
		if (!found)
			abort();
		assert(*found == new);
	}
	struct reporting *report = *found;

	report->refcount++;
	han->reporting = report;

	LOG_DEBUG("Reporting %s refcount++ %d", report->hint, report->refcount);

	return report_action(REPORT_NEW_REQUEST, han, NULL);
}

int report_free_action(struct hsm_action_node *han)
{
	if (!han->reporting)
		return 0;
	han->reporting->refcount--;

	LOG_DEBUG("Reporting %s refcount-- %d", han->reporting->hint,
		  han->reporting->refcount);

	if (han->reporting->refcount != 0)
		return 0;

	reporting_unlink(han->reporting->hint);

	if (!tdelete(han->reporting, &state->reporting_tree, reporting_compare))
		abort();

	free(han->reporting);
	return 0;
}

int report_action(enum report_step step, struct hsm_action_node *han,
		  struct client *client)
{
	char buf[128];
	int rc;

	if (!han->reporting)
		return 0;

	/* Ensure message will generally fit.
	 * (Note it is not a guarantee as we do not enforce client->id max length)
	 * 32 for longest message + fid (u64 + 2 x u32 + [::]) + 32 for client->id */
	static_assert(sizeof(buf) > 32 + 18 + 2 * 10 + 4 + 32,
		      "local buffer too short");
	static_assert(32 > strlen("fid  assigned \n"),
		      "longest message estimation is too short");

	int n;
	switch (step) {
	case REPORT_NEW_REQUEST:
		n = snprintf(buf, sizeof(buf), "new " DFID "\n",
			     PFID(&han->info.dfid));
		break;
	case REPORT_ASSIGN_REQUEST:
		assert(client);
		n = snprintf(buf, sizeof(buf), "assign " DFID " %s\n",
			     PFID(&han->info.dfid), client->id);
		break;
	case REPORT_SEND_REQUEST:
		assert(client);
		n = snprintf(buf, sizeof(buf), "sent " DFID " %s\n",
			     PFID(&han->info.dfid), client->id);
		break;
	case REPORT_MOVER_PROGRESS:
		assert(client);
		n = snprintf(buf, sizeof(buf), "progress %s TODO\n",
			     client->id);
		break;
	}
	if (n < 0) {
		rc = -errno;
		LOG_WARN(rc, "printf failed?");
		return rc;
	}
	if (n >= (int)sizeof(buf)) {
		LOG_WARN(
			-EOVERFLOW,
			"report_action: could not fit message in temporary buffer (wanted %d bytes)",
			n);
		return -EOVERFLOW;
	}

	return reporting_write_to_fs(han->reporting->hint, buf);
}

int report_client(enum report_step step UNUSED, struct client *client UNUSED)
{
	/* XXX not implemented */
	return 0;
}

int reporting_init(void)
{
	int rc;
	bool mkdir_done = false;

	/* reporting disabled */
	if (!state->config.reporting_dir) {
		if (state->config.reporting_hint)
			LOG_WARN(
				-EINVAL,
				"reporting_hint was set without reporting_dir, reporting disabled");
		return 0;
	}
	if (!state->config.reporting_hint) {
		LOG_WARN(
			-EINVAL,
			"reporting_dir was set without reporting_hint, reporting disabled");
		return 0;
	}

	_cleanup_(closep) int mnt_fd = open(state->mntpath, O_RDONLY);
	if (mnt_fd < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not open '%s'", state->mntpath);
		return rc;
	}

again_mkdir:
	state->reporting_dir_fd =
		openat(mnt_fd, state->config.reporting_dir, O_RDONLY);
	if (state->reporting_dir_fd < 0) {
		if (!mkdir_done) {
			rc = mkdirat(mnt_fd, state->config.reporting_dir, 0711);
			if (rc < 0 && errno != EEXIST) {
				rc = -errno;
				LOG_ERROR(
					rc,
					"Could not create '%s' directory in '%s'",
					state->config.reporting_dir,
					state->mntpath);
				return rc;
			}
			mkdir_done = true;
			goto again_mkdir;
		}
		rc = -errno;
		LOG_ERROR(rc, "Could not open '%s' from '%s'",
			  state->config.reporting_dir, state->mntpath);
		return rc;
	}

	return 0;
}

void reporting_cleanup(void)
{
	tdestroy(state->reporting_tree, free);

	if (state->reporting_dir_fd < 0)
		return;

	close(state->reporting_dir_fd);
}
