/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <jansson.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include "protocol.h"
#include "utils.h"

int main(void)
{
	struct lu_fid fid = { 0x4200000000L, 1, 0 };
	json_t *val;
	char *s;

	val = json_fid(&fid);
	s = json_dumps(val, JSON_INDENT(2));
	json_decref(val);

	printf("%s\n", s);

	val = json_loads(s, JSON_ALLOW_NUL, NULL);
	free(s);
	memset(&fid, 0, sizeof(fid));
	assert(json_fid_get(val, &fid) == 0);
	json_decref(val);

	printf(DFID "\n", PFID(&fid));

	struct hsm_action_item *hai, *newhai;
	hai = xcalloc(sizeof(*hai) + 16, 1);
	newhai = xcalloc(sizeof(*hai) + 16, 1);
	hai->hai_action = HSMA_RESTORE;
	hai->hai_fid.f_seq = 0x4200000000L;
	;
	hai->hai_fid.f_oid = 1;
	hai->hai_dfid.f_seq = 0x4200000001L;
	hai->hai_extent.offset = 1;
	hai->hai_extent.length = 0x100000000L;
	hai->hai_cookie = 0x123412341234L;
	hai->hai_gid = 0;
	hai->hai_len = sizeof(*hai) + 16;
	memcpy(hai->hai_data, "test\0test\0", 10);
	val = json_hsm_action_item(hai, 1, 0);
	assert(protocol_setjson_int(val, "extra", 42));
	s = json_dumps(val, JSON_INDENT(2));
	json_decref(val);

	printf("%s\n", s);

	val = json_loads(s, JSON_ALLOW_NUL, NULL);
	free(s);
	assert(json_hsm_action_item_get(val, newhai, sizeof(*hai) + 16, NULL) ==
	       0);
	assert(protocol_getjson_int(val, "extra", 0) == 42);
	assert(protocol_getjson_int(val, "hal_archive_id", 0) == 1);
	assert(protocol_getjson_int(val, "hal_flags", 0) == 0);
	json_decref(val);

	printf("memcmp: %d\n", memcmp(hai, newhai, sizeof(*hai) + 16));

	free(hai);
	free(newhai);

	return 0;
}
