/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <limits.h>
#include <ctype.h>

#include "coordinatool.h"
#include "utils.h"
#include "config_utils.h"

static int config_parse(struct state_config *config, int fail_enoent) {
	int rc = 0;
	FILE *conffile = fopen(config->confpath, "r");
	if (!conffile) {
		if (errno == ENOENT && !fail_enoent) {
			LOG_INFO("Config file %s not found, skipping",
				 config->confpath);
			return 0;
		}
		rc = -errno;
		LOG_ERROR(rc, "Could not open config file %s, aborting",
			  config->confpath);
		return rc;
	}

	char *line = NULL;
	size_t line_size = 0;
	ssize_t n, linenum = 0;
	while (errno = 0, (n = getline(&line, &line_size, conffile)) >= 0) {
		linenum++;
		LOG_DEBUG("Read line %zd: %s", linenum, line);
		char *key = line, *val;
		while (n > 0 && isspace(*key)) {
			key++; n--;
		}
		if (n == 0) /* blank line */
			continue;
		if (*key == '#') /* comment */
			continue;

		/* trailing spaces */
		while (n > 0 && isspace(key[n-1])) {
			key[n-1] = 0;
			n--;
		}
		if (n == 0) // should never happen
			abort();

		ssize_t i = 0;
		while (i < n - 1 && !isspace(key[i])) {
			i++;
		}
		if (i >= n - 1) {
			LOG_WARN(rc, "skipping %s in %s (line %zd) not in 'key value' format",
				 line, config->confpath, linenum);
			continue;
		}
		key[i] = 0;
		i++;
		val = key + i;
		n -= i;
		while (n > 0 && isspace(*val)) {
			val++; n--;
		}
		if (n == 0) {
			LOG_WARN(-EINVAL, "skipping %s in %s (line %zd) not in 'key value' format",
				 line, config->confpath, linenum);
			continue;
		}

		if (!strcasecmp(key, "host")) {
			free((void*)config->host);
			config->host = xstrdup(val);
			LOG_INFO("config setting host to %s", config->host);
			continue;
		}
		if (!strcasecmp(key, "port")) {
			free((void*)config->port);
			config->port = xstrdup(val);
			LOG_INFO("config setting port to %s", config->port);
			continue;
		}
		if (!strcasecmp(key, "redis_host")) {
			free((void*)config->redis_host);
			config->redis_host = xstrdup(val);
			LOG_INFO("config setting redis_host to %s", config->redis_host);
			continue;
		}
		if (!strcasecmp(key, "redis_port")) {
			config->redis_port = parse_int(val, 65535);
			LOG_INFO("config setting redis_port to %d", config->redis_port);
			continue;
		}
		if (!strcasecmp(key, "archive_id")) {
			if (config->archive_cnt >= LL_HSM_MAX_ARCHIVES_PER_AGENT) {
				LOG_ERROR(-E2BIG, "too many archive id given");
				rc = EXIT_FAILURE;
				goto out;
			}
			config->archives[config->archive_cnt] =
				parse_int(val, INT_MAX);
			if (config->archives[config->archive_cnt] <= 0) {
				LOG_ERROR(-ERANGE, "Archive id %s must be > 0", val);
				rc = EXIT_FAILURE;
				goto out;
			}
			config->archive_cnt++;
			continue;
		}
		if (!strcasecmp(key, "client_grace_ms")) {
			config->client_grace_ms = parse_int(val, INT_MAX);
			LOG_INFO("config setting client_grace_ms to %d", config->client_grace_ms);
			continue;
		}
		if (!strcasecmp(key, "verbose")) {
			int intval = str_to_verbose(val);
			if (intval < 0) {
				rc = intval;
				goto out;
			}
			config->verbose = intval;
			llapi_msg_set_level(config->verbose);
			continue;
		}
		/* skip client only options */
		if (!strcasecmp(key, "client_id"))
			continue;
		if (!strcasecmp(key, "max_restore"))
			continue;
		if (!strcasecmp(key, "max_archive"))
			continue;
		if (!strcasecmp(key, "max_remove"))
			continue;
		if (!strcasecmp(key, "hal_size"))
			continue;

		LOG_WARN(-EINVAL, "skipping unknown key %s in %s (line %zd)",
			 key, config->confpath, linenum);

	}
	if (n < 0 && errno != 0) {
		rc = -errno;
		LOG_ERROR(rc, "getline failed reading %s", config->confpath);
	}

out:
	free(line);
	(void)fclose(conffile);
	return rc;

}

int config_init(struct state_config *config) {
	int rc;

	/* first set defaults */
	config->host = xstrdup("coordinatool");
	config->port = xstrdup("5123");
	config->redis_host = xstrdup("localhost");
	config->redis_port = 6379;
	config->client_grace_ms = 10000; /* 10s, double of client reconnect time */
	config->verbose = LLAPI_MSG_NORMAL;
	llapi_msg_set_level(config->verbose);

	/* verbose from env once first to debug config.. */
	rc = getenv_verbose("COORDINATOOL_VERBOSE", &config->verbose);
	if (rc < 0)
		return rc;

	/* then parse config */
	int fail_enoent = true;
	if (!config->confpath) {
		fail_enoent = getenv_str("COORDINATOOL_CONF", &config->confpath);
		if (!fail_enoent) {
			config->confpath = xstrdup("/etc/coordinatool.conf");
		}

	}
	rc = config_parse(config, fail_enoent);
	if (rc)
		return rc;

	/* then overwrite with env */
	getenv_str("COORDINATOOL_HOST", &config->host);
	getenv_str("COORDINATOOL_PORT", &config->port);
	getenv_str("COORDINATOOL_REDIS_HOST", &config->redis_host);
	getenv_int("COORDINATOOL_REDIS_PORT", &config->redis_port);
	getenv_int("COORDINATOOL_CLIENT_GRACE", &config->client_grace_ms);
	rc = getenv_verbose("COORDINATOOL_VERBOSE", &config->verbose);
	if (rc < 0)
		return rc;

	return 0;
}

void config_free(struct state_config *config) {
	free((void*)config->confpath);
	free((void*)config->host);
	free((void*)config->port);
	free((void*)config->redis_host);
}
