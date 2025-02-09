/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef COORDINATOOL_UTILS_H
#define COORDINATOOL_UTILS_H

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UNUSED __attribute__((unused))

#define countof(x) (sizeof(x) / sizeof(*(x)))

/* alloc */
static inline void *xmalloc(size_t size)
{
	void *val = malloc(size);
	if (!val)
		abort();
	return val;
}

static inline void *xcalloc(size_t nmemb, size_t size)
{
	void *val = calloc(nmemb, size);
	if (!val)
		abort();
	return val;
}

static inline void *xrealloc(void *ptr, size_t size)
{
	void *val = realloc(ptr, size);
	if (!val)
		abort();
	return val;
}

static inline char *xstrdup(const char *s)
{
	char *val = strdup(s);
	if (!val)
		abort();
	return val;
}

static inline char *xmemdup0(const char *s, size_t n)
{
	char *val = xmalloc(n+1);
	memcpy(val, s, n);
	val[n] = 0;
	return val;
}

static inline int write_full(int fd, const char *buf, size_t count)
{
	ssize_t n;

	while (count > 0) {
		n = write(fd, buf, count);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			return -errno;
		}
		if ((size_t)n > count) {
			return -ERANGE;
		}
		count -= n;
	}
	return 0;
}

/* time */
#define NS_IN_MSEC 1000000LL
#define NS_IN_SEC 1000000000LL
static inline void ts_from_ns(struct timespec *ts, int64_t ns)
{
	ts->tv_sec = ns / NS_IN_SEC;
	ts->tv_nsec = ns % NS_IN_SEC;
}
static inline int64_t ns_from_ts(struct timespec *ts)
{
	return ts->tv_sec * NS_IN_SEC + ts->tv_nsec;
}

/* get number of ns since epoch as int64
 * (that's good for ~300 years, signed because jansson ints are signed)
 */
static inline int64_t gettime_ns(void)
{
	struct timespec ts;
	int rc;

	rc = clock_gettime(CLOCK_REALTIME, &ts);
	if (rc < 0) {
		/* possible errors should never happen for gettime with
		 * a constant clock id */
		abort();
	}

	return ns_from_ts(&ts);
}

/* parsing */
static inline long parse_int(const char *arg, long max, const char *what)
{
	long rc;
	char *endptr;

	rc = strtol(arg, &endptr, 0);
	if (rc < 0 || rc > max) {
		rc = -ERANGE;
		LOG_ERROR(rc, "%s '%s' is negative or too big (> %ld)", what,
			  arg, max);
	}
	if (*endptr != '\0') {
		rc = -EINVAL;
		LOG_ERROR(rc, "%s '%s' contains (trailing) garbage", what, arg);
	}
	return rc;
}

/* cleanup */
#define _cleanup_(f) __attribute__((cleanup(f)))

static inline void freep(void *ptr)
{
	free(*(void **)ptr);
}

static inline void closep(int *fd)
{
	if (*fd < 3)
		return;
	close(*fd);
	*fd = -1;
}

#endif
