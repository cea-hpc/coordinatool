/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <limits.h>
#include <ctype.h>

#include "coordinatool.h"
#include "utils.h"
#include "config.h"

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
	while ((n = getline(&line, &line_size, conffile)) >= 0) {
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
			rc = -EINVAL;
			LOG_ERROR(rc, "%s in %s (line %zd) not in 'key value' format",
				  line, config->confpath, linenum);
			goto out;
		}
		key[i] = 0;
		i++;
		val = key + i;
		n -= i;
		while (n > 0 && isspace(*val)) {
			val++; n--;
		}
		if (n == 0) {
			rc = -EINVAL;
			LOG_ERROR(rc, "%s in %s (line %zd) not in 'key value' format",
				  line, config->confpath, linenum);
			goto out;
		}

		if (!strcasecmp(key, "host")) {
			free((void*)config->host);
			config->host = xstrdup(val);
			LOG_INFO("config setting host to %s\n", config->host);
			continue;
		}
		if (!strcasecmp(key, "port")) {
			free((void*)config->port);
			config->port = xstrdup(val);
			LOG_INFO("config setting port to %s\n", config->port);
			continue;
		}
		if (!strcasecmp(key, "state_dir_prefix")) {
			free((void*)config->state_dir_prefix);
			config->state_dir_prefix = xstrdup(val);
			LOG_INFO("config setting state dir prefix to %s\n", config->state_dir_prefix);
			continue;
		}
		if (!strcasecmp(key, "verbose")) {
			int intval = str_to_verbose(val);
			if (intval < 0) {
				rc = intval;
				goto out;
			}
			config->verbose = intval;
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
		if (!strcasecmp(key, "archive_id"))
			continue;

		rc = -EINVAL;
		LOG_ERROR(rc, "unknown key %s in %s (line %zd)",
			  key, config->confpath, linenum);
		goto out;

	}
	// XXX check error

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
	config->state_dir_prefix = xstrdup(".coordinatool");
	config->verbose = LLAPI_MSG_NORMAL;
	llapi_msg_set_level(config->verbose);

	/* verbose from env once first to debug config.. */
	rc = getenv_verbose("COORDINATOOL_VERBOSE", &config->verbose);
	if (rc < 0)
		return rc;

	/* then parse config */
	int fail_enoent = false;
	if (!config->confpath) {
		config->confpath = "/etc/coordinatool.conf";
		fail_enoent = getenv_str("COORDINATOOL_CONF", &config->confpath);
	}
	rc = config_parse(config, fail_enoent);
	if (rc)
		return rc;

	/* then overwrite with env */
	getenv_str("COORDINATOOL_HOST", &config->host);
	getenv_str("COORDINATOOL_PORT", &config->port);
	getenv_str("COORDINATOOL_STATE_DIR_PREFIX", &config->state_dir_prefix);
	rc = getenv_verbose("COORDINATOOL_VERBOSE", &config->verbose);
	if (rc < 0)
		return rc;

	return 0;
}

void config_free(struct state_config *config) {
	free((void*)config->host);
	free((void*)config->port);
	free((void*)config->state_dir_prefix);
}
