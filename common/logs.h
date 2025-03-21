/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef COORDINATOOL_LOGS_H
#define COORDINATOOL_LOGS_H

#include <errno.h>
#include <lustre/lustreapi.h>
#include <sys/time.h>
#include <sys/syscall.h>

#define IF_ONCE                       \
	({                            \
		static bool _once;    \
		bool _ret = false;    \
		if (!_once) {         \
			_once = true; \
			_ret = true;  \
		}                     \
		_ret;                 \
	})

#define LOG_WARN_ONCE(_rc, _format, ...) \
	if (IF_ONCE)                     \
	LOG_WARN(_rc, _format, ##__VA_ARGS__)

#define LOG_ERROR(_rc, _format, ...)                                        \
	llapi_error(LLAPI_MSG_ERROR, _rc, "ERROR %s:%d " _format, __FILE__, \
		    __LINE__, ##__VA_ARGS__)

#define LOG_WARN(_rc, _format, ...)                                       \
	llapi_error(LLAPI_MSG_WARN, _rc, "WARN %s:%d " _format, __FILE__, \
		    __LINE__, ##__VA_ARGS__)

#define LOG_NORMAL(_format, ...)                                 \
	llapi_error(LLAPI_MSG_NORMAL | LLAPI_MSG_NO_ERRNO, 0,    \
		    "NORMAL %s:%d " _format, __FILE__, __LINE__, \
		    ##__VA_ARGS__)

#define LOG_INFO(_format, ...)                              \
	llapi_error(LLAPI_MSG_INFO | LLAPI_MSG_NO_ERRNO, 0, \
		    "INFO %s:%d " _format, __FILE__, __LINE__, ##__VA_ARGS__)

#define LOG_DEBUG(_format, ...)                              \
	llapi_error(LLAPI_MSG_DEBUG | LLAPI_MSG_NO_ERRNO, 0, \
		    "DEBUG %s:%d " _format, __FILE__, __LINE__, ##__VA_ARGS__)

static inline const char *ct_action2str(int action)
{
	static char buf[32];

	switch (action) {
	case HSMA_ARCHIVE:
		return "HSMA_ARCHIVE";
	case HSMA_RESTORE:
		return "HSMA_RESTORE";
	case HSMA_REMOVE:
		return "HSMA_REMOVE";
	case HSMA_CANCEL:
		return "HSMA_CANCEL";
	default:
		LOG_ERROR(-EINVAL, "Unknown action: %d", action);
		snprintf(buf, sizeof(buf), "%d", action);
		return buf;
	}
}

#endif
