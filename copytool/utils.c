/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"

int parse_hint(struct hsm_action_node *han, const char *hint_needle,
	       const char **hint, int *hint_len)
{
	const char *data = han->info.data;
	int needle_len;

	needle_len = strlen(hint_needle);
	*hint_len = han_data_len(han);
	*hint = NULL;

	/* can't use strstr on hai data: might contain nul bytes */
	/* note: needle contains the = separator */
	while ((*hint = memmem(data, *hint_len, hint_needle, needle_len))) {
		if (*hint == han->info.data || *hint[-1] == ',')
			break;
		/* false positive, try again */
		*hint_len -= *hint - data;
		data = *hint;
	}
	if (!*hint)
		return 0;

	/* strip matched prefix */
	*hint += needle_len;
	*hint_len -= *hint - data;

	char *hint_end = memchr(*hint, ',', *hint_len);
	if (hint_end) {
		*hint_len = hint_end - *hint;
	}
	
	return 1;
}
