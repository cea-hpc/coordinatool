/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"
#include "config.h"

#include <phobos_store.h>
#include <fcntl.h>
#include <sys/xattr.h>

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
	int nb_new_lock;
	rc = phobos_locate(han->info.hsm_fuid, NULL, 0, NULL, &hostname,
			   &nb_new_lock);
#else
	rc = phobos_locate(han->info.hsm_fuid, NULL, 0, &hostname);
#endif
	if (rc) {
		LOG_ERROR(rc, "phobos: failed to locate " DFID " (oid %s)",
			  PFID(&han->info.dfid), han->info.hsm_fuid);
		return NULL;
	}
	return hostname;
}

int phobos_schedule(struct hsm_action_node *han)
{
	char *hostname = phobos_find_host(han);
	if (hostname == NULL)
		return 0;

	int rc = schedule_on_client(&state->stats.clients, han, hostname) ||
		 schedule_on_client(&state->stats.disconnected_clients, han,
				    hostname);
	free(hostname);
	return rc;
}

bool phobos_can_send(struct client *client, struct hsm_action_node *han)
{
	char *hostname = phobos_find_host(han);
	bool rc = true;

	if (hostname == NULL || !strcmp(client->id, hostname))
		goto out;

	rc = false;

	struct cds_list_head *n;

	cds_list_for_each(n, &state->stats.clients)
	{
		struct client *client =
			caa_container_of(n, struct client, node_clients);

		if (!strcmp(hostname, client->id)) {
			hsm_action_move(&client->queues, han, true);
			goto out;
		}
	}

	/* move the request back into the main queue */
	hsm_action_move(&state->queues, han, true);

out:
	free(hostname);
	return rc;
}
