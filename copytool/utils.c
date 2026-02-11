/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "coordinatool.h"

char *parse_hint(struct hsm_action_node *han, const char *hint_needle,
		 size_t *hint_len)
{
	const char *data = han->info.data;
	char *hint = NULL;
	size_t needle_len;

	needle_len = strlen(hint_needle);
	*hint_len = han_data_len(han);

	/* can't use strstr on hai data: might contain nul bytes */
	/* note: needle contains the = separator */
	while ((hint = memmem(data, *hint_len, hint_needle, needle_len))) {
		if (hint == han->info.data || hint[-1] == ',')
			break;
		/* false positive, try again */
		*hint_len -= hint - data;
		if (*hint_len == 0)
			return NULL;
		*hint_len -= 1;
		data = hint + 1;
	}
	if (!hint)
		return NULL;

	/* strip matched prefix */
	hint += needle_len;
	*hint_len -= hint - data;

	char *hint_end = memchr(hint, ',', *hint_len);
	if (hint_end) {
		*hint_len = hint_end - hint;
	}

	return hint;
}

size_t dbj2(const char *buf, size_t size)
{
	size_t hash = 5381;

	for (size_t i = 0; i < size; i++)
		hash = ((hash << 5) + hash) + buf[i];

	return hash;
}

char *replace_string(const char *orig, size_t orig_len, const char *new_value,
		     size_t new_len, const char *old_value, size_t old_len)
{
	char *string, *ptr;
	size_t len;

	len = new_len + (old_value - orig) +
	      ((orig + orig_len) - (old_value + old_len));

	string = xmalloc(len + 1);
	string[len] = '\0';

	ptr = string;
	memcpy(ptr, orig, old_value - orig);
	ptr += (old_value - orig);

	memcpy(ptr, new_value, new_len);
	ptr += new_len;

	memcpy(ptr, old_value + old_len,
	       ((orig + orig_len) - (old_value + old_len)));

	return string;
}
