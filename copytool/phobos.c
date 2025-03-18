/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <fcntl.h>
#include <sys/xattr.h>

#include "config.h"

#include "coordinatool.h"
#include "coordinatool_phobos_store.h"

int phobos_enrich(struct hsm_action_node *han)
{
	char oid[XATTR_SIZE_MAX + 1];
	int rc, save_errno, fd;
	ssize_t oidlen;

	/* only enrich restore */
	if (han->info.action != HSMA_RESTORE)
		return 0;

	fd = llapi_open_by_fid(state->mntpath, &han->info.dfid,
			       O_RDONLY | O_NOATIME | O_NOFOLLOW);
	if (fd < 0) {
		rc = -errno;
		LOG_WARN(rc, "Could not open " DFID " (phobos enrich)",
			 PFID(&han->info.dfid));
		return rc;
	}

	oidlen = fgetxattr(fd, "trusted.hsm_fuid", oid, XATTR_SIZE_MAX);
	save_errno = errno;
	close(fd);
	if (oidlen < 0) {
		/* missing xattr, not using with phobos? */
		if (save_errno == ENODATA || save_errno == ENOTSUP)
			return 0;

		rc = -save_errno;
		LOG_WARN(rc, "Could not getxattr trusted.hsm_fuid " DFID,
			 PFID(&han->info.dfid));
		return rc;
	}

	oid[oidlen] = '\0';
	han->info.hsm_fuid = xstrdup(oid);

	return 0;
}

static char *phobos_find_host(struct hsm_action_node *han)
{
	int rc;
	char *hostname;

	/* only enrich restore */
	if (han->info.action != HSMA_RESTORE)
		return NULL;

	/* not in phobos */
	if (!han->info.hsm_fuid)
		return NULL;

#if HAVE_PHOBOS_1_95
	/* pick least busy host for focus host in case it helps */
	struct client *client;
	const char *focus_host = NULL;
	int min_busy = INT_MAX;
	cds_list_for_each_entry(client, &state->stats.clients, node_clients)
	{
		if (client->current_restore < min_busy) {
			min_busy = client->current_restore;
			focus_host = client->id;
		} else if (client->current_restore == min_busy) {
			/* not a fair random pick but doesn't matter */
			if (rand() % 1000 > 500)
				focus_host = client->id;
		}
	}
	int nb_new_lock;
	rc = phobos_locate(han->info.hsm_fuid, NULL, 0, focus_host, &hostname,
			   &nb_new_lock);
#else
	rc = phobos_locate(han->info.hsm_fuid, NULL, 0, &hostname);
#endif
	if (rc) {
		LOG_ERROR(rc, "phobos: failed to locate " DFID " (oid %s)",
			  PFID(&han->info.dfid), han->info.hsm_fuid);
		return NULL;
	}
	LOG_DEBUG("phobos: locate " DFID " on %s", PFID(&han->info.dfid),
		  hostname ?: "(null)");
	return hostname;
}

struct cds_list_head *phobos_schedule(struct hsm_action_node *han)
{
	char *hostname = phobos_find_host(han);
	if (hostname == NULL)
		return NULL;

	struct client *client = find_client(&state->stats.clients, hostname);
	if (!client) {
		client = find_client(&state->stats.disconnected_clients,
				     hostname);
	}
	if (!client) {
		LOG_WARN(-ENOENT,
			 "phobos: locate " DFID
			 " requested %s, but no such host found",
			 PFID(&han->info.dfid), hostname);
		return NULL;
	}
	return schedule_on_client(client, han);
}

bool phobos_can_send(struct client *client, struct hsm_action_node *han)
{
	char *hostname = phobos_find_host(han);
	bool rc = true;

	if (hostname == NULL || !strcmp(client->id, hostname))
		goto out;

	LOG_NORMAL("phobos: " DFID
		   " locate requested %s, refusing to send to %s",
		   PFID(&han->info.dfid), hostname, client->id);
	rc = false;

	struct cds_list_head *n, *found = NULL;

	cds_list_for_each(n, &state->stats.clients)
	{
		struct client *client =
			caa_container_of(n, struct client, node_clients);

		if (!strcmp(hostname, client->id)) {
			found = schedule_on_client(client, han);
			break;
		}
	}
	if (!found)
		found = get_queue_list(&state->queues, han);
	assert(found);

	/* move the request back into the main queue */
	/* XXX: cannot report errors back.. */
	hsm_action_requeue(han, found);

out:
	free(hostname);
	return rc;
}
