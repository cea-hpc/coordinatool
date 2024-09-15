/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"
#include "config.h"

#include <phobos_store.h>
#include <fcntl.h>
#include <sys/xattr.h>

int phobos_enrich(struct state *state,
		  struct hsm_action_node *han) {
	char oid[XATTR_SIZE_MAX + 1];
	int rc, save_errno, fd;
	char *hostname;
	ssize_t oidlen;

	han->info.hsm_fuid = NULL;
	if (han->info.action != HSMA_RESTORE)
		/* only enrich restore */
		return 0;

	fd = llapi_open_by_fid(state->mntpath, &han->info.dfid,
			       O_RDONLY | O_NOATIME | O_NOFOLLOW);
	if (fd < 0)
		return -errno;

	oidlen = fgetxattr(fd, "trusted.hsm_fuid", oid, XATTR_SIZE_MAX);
	save_errno = errno;
	close(fd);
	if (oidlen < 0)
		return (save_errno == ENODATA || save_errno == ENOTSUP) ?
			0 : -save_errno;

	oid[oidlen] = '\0';

#if HAVE_PHOBOS_1_95
	int nb_new_lock;
	rc = phobos_locate(oid, NULL, 0, NULL, &hostname, &nb_new_lock);
#else
	rc = phobos_locate(oid, NULL, 0, &hostname);
#endif
	if (rc)
		return rc;

	han->info.hsm_fuid = xstrdup(oid);
	if (hostname == NULL)
		return 0;

	return schedule_on_client(&state->stats.clients, han, hostname)
		|| schedule_on_client(&state->stats.disconnected_clients, han, hostname);
}

bool phobos_can_send(struct client *client,
		     struct hsm_action_node *han) {
	char *hostname;
	int rc;

	if (han->info.action != HSMA_RESTORE)
		/* only restore are enriched */
		return true;

	// TODO If we just received the request, we don't need to do a locate
	// here.
#if HAVE_PHOBOS_1_95
	int nb_new_lock;
	rc = phobos_locate(han->info.hsm_fuid, NULL, 0, NULL, &hostname,
			   &nb_new_lock);
#else
	rc = phobos_locate(han->info.hsm_fuid, NULL, 0, &hostname);
#endif
	if (rc)
		/* do not prevent sending a request if Phobos fails */
		return true;


	if (hostname == NULL || !strcmp(client->id, hostname))
		return true;

	struct cds_list_head *n;

	cds_list_for_each(n, &client->queues.state->stats.clients) {
		struct client *client =
			caa_container_of(n, struct client, node_clients);

		if (!strcmp(hostname, client->id)) {
			hsm_action_move(&client->queues, han, true);
			return false;
		}
	}

	/* move the request back into the main queue */
	hsm_action_move(&han->queues->state->queues, han, true);

	return false;
}
