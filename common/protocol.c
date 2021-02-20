/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <errno.h>

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
int json_hsm_action_item_get(json_t *json, struct hsm_action_item *hai) {
	char *data;
	size_t data_len;
	unsigned int hai_len = hai->hai_len;

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

	hai->hai_len = sizeof(*hai) + data_len;
	/* round up to next byte */
	hai->hai_len += 7;
	hai->hai_len -= hai->hai_len % 8;
	if (hai_len < hai->hai_len) {
		free(data);
		return -EOVERFLOW;
	}

	memcpy(hai->hai_data, data, data_len);
	memset(hai->hai_data + data_len, 0,
	       hai->hai_len - sizeof(*hai) - data_len);

	return 0;
}

int json_hsm_action_list_get(json_t *json, struct hsm_action_list *hal);
