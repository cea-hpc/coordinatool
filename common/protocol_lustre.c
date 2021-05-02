/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>
#include <lustre/lustreapi.h>

#include "protocol.h"

json_t *json_fid(struct lu_fid *fid) {
	return json_pack("{sI,si,si}", "f_seq", fid->f_seq,
			 "f_oid", fid->f_oid, "f_ver", fid->f_ver);
}
int json_fid_get(json_t *json, struct lu_fid *fid) {
	return json_unpack(json, "{sI,si,si!}", "f_seq", &fid->f_seq,
			   "f_oid", &fid->f_oid, "f_ver", &fid->f_ver);
}

json_t *json_hsm_action_item(struct hsm_action_item *hai) {
	return json_pack("{si,so,so,sI,sI,sI,sI,ss#}",
			 "hai_action", hai->hai_action,
			 "hai_fid", json_fid(&hai->hai_fid),
			 "hai_dfid", json_fid(&hai->hai_dfid),
			 "hai_extent_offset", hai->hai_extent.offset,
			 "hai_extent_length", hai->hai_extent.length,
			 "hai_cookie", hai->hai_cookie,
			 "hai_gid", hai->hai_gid,
			 "hai_data", hai->hai_data,
			 hai->hai_len - sizeof(*hai));
}
int json_hsm_action_item_get(json_t *json, struct hsm_action_item *hai, size_t hai_len) {
	char *data;
	size_t data_len;

	if (hai_len < sizeof(*hai))
		return -EINVAL;

	if (json_unpack(json,
			"{si,s{sI,si,si!},s{sI,si,si!},sI,sI,sI,sI,ss%!}",
			 "hai_action", &hai->hai_action,
			 "hai_fid", "f_seq", &hai->hai_fid.f_seq,
				    "f_oid", &hai->hai_fid.f_oid,
				    "f_ver", &hai->hai_fid.f_ver,
			 "hai_dfid", "f_seq", &hai->hai_dfid.f_seq,
				     "f_oid", &hai->hai_dfid.f_oid,
				     "f_ver", &hai->hai_dfid.f_ver,
			 "hai_extent_offset", &hai->hai_extent.offset,
			 "hai_extent_length", &hai->hai_extent.length,
			 "hai_cookie", &hai->hai_cookie,
			 "hai_gid", &hai->hai_gid,
			 "hai_data", &data, &data_len) != 0)
		return -1;

	hai->hai_len = __ALIGN_KERNEL(sizeof(*hai) + data_len, 8);
	if (hai_len < hai->hai_len) {
		return -EOVERFLOW;
	}

	memcpy(hai->hai_data, data, data_len);
	memset(hai->hai_data + data_len, 0,
	       hai->hai_len - sizeof(*hai) - data_len);

	return 0;
}

int json_hsm_action_list_get(json_t *json, struct hsm_action_list *hal,
			     size_t hal_len) {
	struct hsm_action_item *hai;
	unsigned int count;
	int rc;
	char *fsname;
	size_t fsname_len, len;
	json_t *json_list, *item;

	if (hal_len < sizeof(*hal))
		return -EINVAL;
	hal_len -= sizeof(*hal);

	if (json_unpack(json,
			"{si,si,sI,si,ss%,so!}",
			"hal_version", &hal->hal_version,
			"hal_count", &hal->hal_count,
			"hal_flags", &hal->hal_flags,
			"hal_archive_id", &hal->hal_archive_id,
			"hal_fsname", &fsname, &fsname_len,
			"list", &json_list) != 0)
		return -1;

	if (hal->hal_version != HAL_VERSION) {
		rc = -EINVAL;
		LOG_ERROR(rc, "hal_version was %d, expecting %d",
			  hal->hal_version, HAL_VERSION);
		return rc;
	}
	len = __ALIGN_KERNEL(fsname_len + 1, 8);
	if (hal_len < len)
		return -EINVAL;
	hal_len -= len;
	/* guaranteed to be nul-terminated by jansson and len is +1 */
	strncpy(hal->hal_fsname, fsname, len);

	hai = hai_first(hal);
	json_array_foreach(json_list, count, item) {
		if ((rc = json_hsm_action_item_get(item, hai, hal_len)) < 0)
			return rc;
		hai = hai_next(hai);
	}

	if (hal->hal_count != count) {
		rc = -EINVAL;
		LOG_ERROR(rc, "Expected %u items got %u in hsm action list",
			  hal->hal_count, count);
		return rc;
	}

	return count;
}
