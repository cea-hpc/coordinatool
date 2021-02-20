/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef MASTER_CT_LUSTRE_H
#define MASTER_CT_LUSTRE_H

#include <lustre/lustreapi.h>

typedef int (*parse_request_cb)(struct hsm_action_item *hai,
				unsigned int archive_id, unsigned long flags,
				void *arg);

/* returns number of requests parsed, or negative errno on error
 *
 * @param fd an open file descriptor to a file structured like lustre
 * hsm/active_requests file. It is read linearily and not seeked nor closed.
 * @param cb callback to call on each hsm_action_item of the file
 * @param cb_arg opaque arg passed to the calblack
 * @return number of requests parsed if >= 0, -errno on error
 */
int parse_active_requests(int fd, parse_request_cb cb, void *cb_arg);

#endif
