/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef COORDINATOOL_UTILS_H
#define COORDINATOOL_UTILS_H

#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* get number of ns since epoch as int64
 * (that's good for ~300 years, signed because jansson ints are signed)
 */
static inline int64_t gettime_ns(void) {
	struct timespec ts;
	int rc;

	rc = clock_gettime(CLOCK_REALTIME, &ts);
	if (rc < 0) {
		/* possible errors should never happen for gettime with
		 * a constant clock id */
		abort();
	}

	return ts.tv_sec * 1000000000 + ts.tv_nsec;
}

#endif
