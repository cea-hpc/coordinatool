/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <limits.h>
#include <stdlib.h>
#include <strings.h>

#include "logs.h"

int getenv_str(const char *name, const char **val) {
	const char *env = getenv(name);
	if (!env)
		return 0;
	*val = env;
	LOG_INFO("env setting %s to %s", name, env);
	return 1;
}

long long str_suffix_to_u32(const char *str, const char *error_hint) {
	char *endptr;

	long long val = strtoll(str, &endptr, 0);
	long multiplier = 1;

	if (!endptr)
		abort();
	switch (*endptr) {
	case 0:
		break;
	case 'g':
	case 'G':
		multiplier *= 1024;
		// fallthrough
	case 'm':
	case 'M':
		multiplier *= 1024;
		// fallthrough
	case 'k':
	case 'K':
		multiplier *= 1024;
		if (endptr[1] != 0) {
			LOG_WARN(-EINVAL, "trailing data after size prefix: %s, continuing anyway",
				  endptr + 1);
		}
		break;
	default:
		LOG_ERROR(-EINVAL, "%s was set to %s, which has trailing %s",
			  error_hint, str, endptr);
		return -EINVAL;
	}

	/* allow -1 as max */
	if (val == -1)
		return UINT32_MAX;

	if (val > UINT32_MAX / multiplier || val < 0) {
		LOG_ERROR(-EINVAL, "%s was set to %s, which would overflow",
			  error_hint, str);
		return -EINVAL;
	}
	return val * multiplier;
}

int getenv_u32(const char *name, uint32_t *val) {
	const char *env = getenv(name);
	if (!env)
		return 0;

	long long envval = str_suffix_to_u32(env, name);
	if (envval < 0)
		return envval;

	*val = envval;
	LOG_INFO("env setting %s to %u", name, *val);
	return 1;
}

int getenv_int(const char *name, int *val) {
	const char *env = getenv(name);
	if (!env)
		return 0;

	char *endptr;
	long long envval = strtol(env, &endptr, 0);
	if (envval < 0 || envval > INT_MAX) {
		LOG_ERROR(-EINVAL, "env %s (%s) not an int", name, env);
		return -EINVAL;
	}

	*val = envval;
	LOG_INFO("env setting %s to %u", name, *val);
	return 1;
}


enum llapi_message_level str_to_verbose(const char *str) {
	if (!strcasecmp(str, "off")) {
		return LLAPI_MSG_OFF;
	}
	if (!strcasecmp(str, "fatal")) {
		return LLAPI_MSG_FATAL;
	}
	if (!strcasecmp(str, "error")) {
		return LLAPI_MSG_ERROR;
	}
	if (!strcasecmp(str, "warn")) {
		return LLAPI_MSG_WARN;
	}
	if (!strcasecmp(str, "normal")) {
		return LLAPI_MSG_NORMAL;
	}
	if (!strcasecmp(str, "info")) {
		return LLAPI_MSG_INFO;
	}
	if (!strcasecmp(str, "debug")) {
		return LLAPI_MSG_DEBUG;
	}
	LOG_ERROR(-EINVAL, "invalid debug level: %s", str);
	return -1;
}

int getenv_verbose(const char *name, int *val) {
	const char *env = getenv(name);
	if (!env)
		return 0;

	int verbose = str_to_verbose(env);
	if (verbose < 0)
		return verbose;

	*val = verbose;
	llapi_msg_set_level(verbose);
	return 1;
}


