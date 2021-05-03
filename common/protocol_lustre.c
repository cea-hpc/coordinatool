/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>
#include <lustre/lustreapi.h>

#include "protocol.h"

json_t *json_fid(struct lu_fid *fid) {
	return json_pack("{sI,si,si}", "f_seq", fid->f_seq,
			 "f_oid", fid->f_oid, "f_ver", fid->f_ver);
}

struct lu_fid_unpacked {
	__u64 f_seq;
	__u32 f_oid;
	__u32 f_ver;
};

int json_fid_get(json_t *json, struct lu_fid *fid) {
	int rc;
	struct lu_fid_unpacked unpack;

	rc = json_unpack(json, "{sI,si,si!}", "f_seq", &unpack.f_seq,
			 "f_oid", &unpack.f_oid, "f_ver", &unpack.f_ver);
	if (rc)
		return rc;
	fid->f_seq = unpack.f_seq;
	fid->f_oid = unpack.f_oid;
	fid->f_ver = unpack.f_ver;
	return 0;
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

/* need to parse in local, nonpacked fields then copy to avoid
 * unaligned accesses... */
struct hsm_action_item_unpacked {
        __u32      hai_len;     /* valid size of this struct */
        __u32      hai_action;  /* hsm_copytool_action, but use known size */
        struct lu_fid hai_fid;     /* Lustre FID to operate on */
        struct lu_fid hai_dfid;    /* fid used for data access */
        struct hsm_extent hai_extent;  /* byte range to operate on */
        __u64      hai_cookie;  /* action cookie from coordinator */
        __u64      hai_gid;     /* grouplock id */
};

int json_hsm_action_item_get(json_t *json, struct hsm_action_item *hai, size_t hai_len) {
	char *data;
	size_t data_len;
	struct hsm_action_item_unpacked unpack;

	if (hai_len < sizeof(*hai))
		return -EINVAL;

	if (json_unpack(json,
			"{si,s{sI,si,si!},s{sI,si,si!},sI,sI,sI,sI,ss%!}",
			 "hai_action", &unpack.hai_action,
			 "hai_fid", "f_seq", &unpack.hai_fid.f_seq,
				    "f_oid", &unpack.hai_fid.f_oid,
				    "f_ver", &unpack.hai_fid.f_ver,
			 "hai_dfid", "f_seq", &unpack.hai_dfid.f_seq,
				     "f_oid", &unpack.hai_dfid.f_oid,
				     "f_ver", &unpack.hai_dfid.f_ver,
			 "hai_extent_offset", &unpack.hai_extent.offset,
			 "hai_extent_length", &unpack.hai_extent.length,
			 "hai_cookie", &unpack.hai_cookie,
			 "hai_gid", &unpack.hai_gid,
			 "hai_data", &data, &data_len) != 0)
		return -EINVAL;

	hai->hai_action = unpack.hai_action;
	hai->hai_fid.f_seq = unpack.hai_fid.f_seq;
	hai->hai_fid.f_oid = unpack.hai_fid.f_oid;
	hai->hai_fid.f_ver = unpack.hai_fid.f_ver;
	hai->hai_dfid.f_seq = unpack.hai_dfid.f_seq;
	hai->hai_dfid.f_oid = unpack.hai_dfid.f_oid;
	hai->hai_dfid.f_ver = unpack.hai_dfid.f_ver;
	hai->hai_extent.offset = unpack.hai_extent.offset;
	hai->hai_extent.length = unpack.hai_extent.length;
	hai->hai_cookie = unpack.hai_cookie;
	hai->hai_gid = unpack.hai_gid;

	hai->hai_len = __ALIGN_KERNEL_MASK(sizeof(*hai) + data_len, 7);
	if (hai_len < hai->hai_len) {
		return -EOVERFLOW;
	}

	memcpy(hai->hai_data, data, data_len);
	memset(hai->hai_data + data_len, 0,
	       hai->hai_len - sizeof(*hai) - data_len);

	return 0;
}

struct hsm_action_list_unpacked {
        __u32 hal_version;
        __u32 hal_count;       /* number of hai's to follow */
        __u64 hal_compound_id; /* returned by coordinator, ignored */
        __u64 hal_flags;
	__u32 hal_archive_id;
};

int json_hsm_action_list_get(json_t *json, struct hsm_action_list *hal,
			     size_t hal_len) {
	struct hsm_action_item *hai;
	unsigned int count;
	int rc;
	char *fsname;
	size_t fsname_len, len;
	json_t *json_list, *item;
	struct hsm_action_list_unpacked unpack;

	if (hal_len < sizeof(*hal))
		return -EINVAL;
	hal_len -= sizeof(*hal);

	if (json_unpack(json,
			"{si,si,sI,si,ss%,so!}",
			"hal_version", &unpack.hal_version,
			"hal_count", &unpack.hal_count,
			"hal_flags", &unpack.hal_flags,
			"hal_archive_id", &unpack.hal_archive_id,
			"hal_fsname", &fsname, &fsname_len,
			"list", &json_list) != 0)
		return -EINVAL;
	hal->hal_version = unpack.hal_version;
	hal->hal_count = unpack.hal_count;
	hal->hal_flags = unpack.hal_flags;
	hal->hal_archive_id = unpack.hal_archive_id;

	if (hal->hal_version != HAL_VERSION) {
		rc = -EINVAL;
		LOG_ERROR(rc, "hal_version was %d, expecting %d",
			  hal->hal_version, HAL_VERSION);
		return rc;
	}
	len = __ALIGN_KERNEL_MASK(fsname_len + 1, 7);
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
