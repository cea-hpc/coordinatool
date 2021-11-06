/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef COORDINATOOL_UTILS_H
#define COORDINATOOL_UTILS_H

#define UNUSED __attribute__((unused))

static inline void *xmalloc(size_t size) {
	void *val = malloc(size);
	if (!val)
		abort();
	return val;
}

static inline void *xcalloc(size_t nmemb, size_t size) {
	void *val = calloc(nmemb, size);
	if (!val)
		abort();
	return val;
}

static inline char *xstrdup(const char *s) {
	char *val = strdup(s);
	if (!val)
		abort();
	return val;
}

#endif
