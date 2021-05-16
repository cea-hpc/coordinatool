/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <stdio.h>

#include "lustre.h"
#include "logs.h"

int parse_hai_cb(struct hsm_action_item *hai, unsigned int archive_id,
		 unsigned long flags, void *arg) {
	(void)arg;
	unsigned long len = hai->hai_len - sizeof(*hai);

	printf("got hai fid="DFID" dfid="DFID" cookie=%#llx action=%s extent=%#llx-%#llx gid=%#llx ",
	       PFID(&hai->hai_fid), PFID(&hai->hai_dfid), hai->hai_cookie,
	       ct_action2str(hai->hai_action), hai->hai_extent.offset,
	       hai->hai_extent.length, hai->hai_gid);
	if (len)
		printf("data=%*s ",
		       (int)(hai->hai_len - sizeof(*hai)), hai->hai_data);
	printf("archive#=%u flags=%#lx\n",
	       archive_id, flags);

	return 0;
}

int main() {
	int rc;

	rc = parse_active_requests(0, parse_hai_cb, NULL);
	printf("got %d items\n", rc);

	return 0;
}
