/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "lustre.h"
#include "logs.h"

#define READ_CHUNK 10240

#include <stdio.h>

static int parse_fid(char *fid_str, struct lu_fid *fid) {
	char *end;
	fid->f_seq = strtoull(fid_str, &end, 0);
	if (*end != ':')
		return -EINVAL;
	end++;
	fid->f_oid = strtoul(end, &end, 0);
	if (*end != ':')
		return -EINVAL;
	end++;
	fid->f_ver = strtoul(end, &end, 0);
	return end - fid_str;
}

/* helper to make sure the keyword is a word start */
static inline char *find_keyword(char *line, char *keyword) {
	char *item, *orig_line=line;

	while (*line) {
		item = strstr(line, keyword);
		if (!item)
			goto err_out;
		if (item == line || item[-1] == ' ')
			return item + strlen(keyword);
		line = item + 1;
	}
err_out:
	LOG_ERROR(-EINVAL, "Keyword %s not found in \"%s\"",
		  keyword, orig_line);
	return NULL;
}

static int parse_active_request_line(char *line, parse_request_cb cb,
				     void *cb_arg) {
	char *item, *end;
	int rc = -EINVAL, n;
	size_t len, i;
	struct hsm_action_item *hai;
	unsigned int archive_id;
	unsigned long flags;
	unsigned long long tmp1, tmp2;

	/* parsing is done in two steps:
	 * - first fine data to allocate proper length
	 * - then fill the rest
	 */
	item = find_keyword(line, "data=[");
	if (!item)
		return rc;
	end = strchr(item, ']');
	if (!end) {
		LOG_ERROR(rc, "No end delimiter ] for data field");
		return rc;
	}
	len = (end-item)/2;
	n = __ALIGN_KERNEL_MASK(sizeof(*hai) + len, 7);
	hai = malloc(n);
	hai->hai_len = n;
	for (i=0; i < len; i++) {
		n = sscanf(item, "%2hhX", (unsigned char*)&hai->hai_data[i]);
		if (n != 1) {
			LOG_ERROR(rc, "scanf failed to read hex from %s",
			          item);
			goto out;
		}
		item += 2;
	}
	memset(hai->hai_data + len, 0, hai->hai_len - sizeof(*hai) - len);

	/* action */
	item = find_keyword(line, "action=");
	if (!item)
		goto out;
	if (strncmp(item, "RESTORE ", 8) == 0) {
		hai->hai_action = HSMA_RESTORE;
	} else if (strncmp(item, "ARCHIVE ", 8) == 0) {
		hai->hai_action = HSMA_ARCHIVE;
	} else if (strncmp(item, "REMOVE ", 7) == 0) {
		hai->hai_action = HSMA_REMOVE;
	} else {
		LOG_ERROR(rc, "Unknown action %s in \"%s\"", item, line);
		goto out;
	}

	/* fid */
	item = find_keyword(line, "fid=[");
	if (!item)
		goto out;
	n = parse_fid(item, &hai->hai_fid);
	if (n < 0 || item[n] != ']') {
		LOG_ERROR(rc, "fid is invalid: %s", item);
	}

	/* dfid */
	item = find_keyword(line, "dfid=[");
	if (!item)
		goto out;
	n = parse_fid(item, &hai->hai_dfid);
	if (n < 0 || item[n] != ']') {
		LOG_ERROR(rc, "dfid is invalid: %s", item);
	}

	/* extent offset/length */
	item = find_keyword(line, "extent=");
	if (!item)
		goto out;
	/* (use temp values to avoid unaligned accesses) */
	n = sscanf(item, "%llx-%llx", &tmp1, &tmp2);
	if (n != 2) {
		LOG_ERROR(rc, "scanf failed to read offset start/end from %s",
			  item);
		goto out;
	}
	hai->hai_extent.offset = tmp1;
	hai->hai_extent.length = tmp2;

	/* cookie */
	item = find_keyword(line, "compound/cookie=");
	if (!item)
		goto out;
	n = sscanf(item, "%*x/%llx", &tmp1);
	if (n != 1) {
		LOG_ERROR(rc, "scanf failed to read compound/cookie from %s",
			  item);
		goto out;
	}
	hai->hai_cookie = tmp1;

	/* gid */
	item = find_keyword(line, "gid=");
	if (!item)
		goto out;
	hai->hai_gid = strtol(item, NULL, 0);
	// XXX check (use parse_int?)

	/* archive# */
	item = find_keyword(line, "archive#=");
	if (!item)
		goto out;
	archive_id = strtol(item, NULL, 0);

	/* flags */
	item = find_keyword(line, "flags=");
	if (!item)
		goto out;
	flags = strtol(item, NULL, 0);

	/* XXX canceled, done?, uuid? */

	rc = cb(hai, archive_id, flags, cb_arg);

out:
	free(hai);

	return rc;
}

int parse_active_requests(int fd, parse_request_cb cb, void *cb_arg) {
	char buffer[READ_CHUNK];
	char *line = buffer, *end = buffer, *newline;
	int n, rc = 0;

	while (true) {
		if (end - buffer > READ_CHUNK - 500) {
			if (line == buffer) {
				rc = -E2BIG;
				LOG_ERROR(rc, "No new line in %zd bytes?",
					  end - buffer);
				break;
			}
			n = end - line;
			memmove(buffer, line, n);
			line = buffer;
			end = line + n;
		}
		n = read(fd, end, READ_CHUNK - (end - buffer));
		if (n == 0 && line != end) {
			rc = -EINVAL;
			LOG_ERROR(rc, "trailing text at end of file: %s",
				  line);
			break;
		}
		if (n == 0)
			break;
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			rc = -errno;
			LOG_ERROR(rc, "read error");
			break;
		}
		end += n;
		*end = 0;
		while ((newline = strchr(line, '\n'))) {
			*newline = 0;
			n = parse_active_request_line(line, cb, cb_arg);
			if (n < 0)
				return n;
			rc++;
			line = newline + 1;
		}
	}

	return rc;
}

