/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/select.h>
#include <linux/lustre/lustre_idl.h>

#include "logs.h"

int ct_start(const char *mntpath, int archive_cnt, int *archive_id) {
	struct hsm_copytool_private *ctdata;
	int hsm_fd;
	int rc;

	rc = llapi_hsm_copytool_register(&ctdata, mntpath, archive_cnt,
					 archive_id, 0); 
	if (rc < 0) {
		LOG_ERROR(rc, "cannot start copytool interface");
		return rc;
	}
	hsm_fd = llapi_hsm_copytool_get_fd(ctdata);
	if (hsm_fd < 0) {
		LOG_ERROR(hsm_fd, "cannot get kuc fd after hsm registration");
		return hsm_fd;
	}
	while (1) {
		fd_set rfds;
		int maxfd = hsm_fd +1;

		FD_ZERO(&rfds);
		LOG_DEBUG("got hsm fd: %d", hsm_fd);
		FD_SET(hsm_fd, &rfds);


		rc = select(maxfd, &rfds, NULL, NULL, NULL);
		if (rc < 0) {
			LOG_ERROR(-rc, "select error");
			return -rc;
		}
		if (rc == 0)
			continue;
		if (FD_ISSET(hsm_fd, &rfds)) {
			struct hsm_action_list *hal;
			int msgsize;

			rc = llapi_hsm_copytool_recv(ctdata, &hal, &msgsize);
			if (rc == -ESHUTDOWN) {
				LOG_INFO("shutting down");
				break;
			}
			if (rc < 0) {
				LOG_ERROR(rc, "Could not recv hsm message");
				return rc;
			}
			LOG_DEBUG("copytool fs=%s, archive#=%d, item_count=%d",
				  hal->hal_fsname, hal->hal_archive_id,
				  hal->hal_count);
			// XXX match fsname with known one?

			struct hsm_action_item *hai = hai_first(hal);
			int i = 0;
			while (++i <= hal->hal_count) {
				LOG_DEBUG("item %d: %s on "DFID ,
					  i, llapi_hsm_action2str(hai->hai_action),
					  PFID(&hai->hai_fid));
				hai = hai_next(hai);
			}
		}


	}
}

int main(int argc, char *argv[]) {
	struct option long_opts[] = {
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet",   no_argument, NULL, 'q' },
		{ "archive", required_argument, NULL, 'A' },
		{ 0 },
	};
	int rc;
	int verbose = LLAPI_MSG_INFO;
	const char *mntpath = NULL;
	int archive_cnt = 0;
	int archive_id[LL_HSM_MAX_ARCHIVES_PER_AGENT];

	while ((rc = getopt_long(argc, argv, "vqA:",
			         long_opts, NULL)) != -1) {
		switch (rc) {
		case 'A':
			if (archive_cnt >= LL_HSM_MAX_ARCHIVES_PER_AGENT) {
				LOG_ERROR(-E2BIG, "too many archive id given");
				return EXIT_FAILURE;
			}
			archive_id[archive_cnt] = atoi(optarg);
			archive_cnt++;
		case 'v':
			verbose++;
			break;
		case 'q':
			verbose--;
			break;
		}
	}
	if (argc != optind + 1) {
		LOG_ERROR(-EINVAL, "no mount point specified");
		return EXIT_FAILURE;
	}
	mntpath = argv[optind];

	llapi_msg_set_level(verbose);

	rc = ct_start(mntpath, archive_cnt, archive_id);
	if (rc)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
