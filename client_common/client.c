/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <limits.h>
#include <ctype.h>

#include "client_common.h"
#include "utils.h"
#include "config.h"

static int config_parse(struct ct_state_config *config, int fail_enoent) {
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
			LOG_INFO("config setting host to %s", config->host);
			continue;
		}
		if (!strcasecmp(key, "port")) {
			free((void*)config->port);
			config->port = xstrdup(val);
			LOG_INFO("config setting port to %s", config->port);
			continue;
		}
		if (!strcasecmp(key, "client_id")) {
			free((void*)config->client_id);
			config->client_id = xstrdup(val);
			LOG_INFO("config setting state dir prefix to %s", config->client_id);
			continue;
		}
		if (!strcasecmp(key, "max_restore")) {
			long long intval = str_suffix_to_u32(val, "max_restore");
			if (intval < 0) {
				rc = intval;
				goto out;
			}
			config->max_restore = intval;
			LOG_INFO("config setting max_restore to %u",
				 config->max_restore);
			continue;
		}
		if (!strcasecmp(key, "max_archive")) {
			long long intval = str_suffix_to_u32(val, "max_archive");
			if (intval < 0) {
				rc = intval;
				goto out;
			}
			config->max_archive = intval;
			LOG_INFO("config setting max_archive to %u",
				 config->max_archive);
			continue;
		}
		if (!strcasecmp(key, "max_remove")) {
			long long intval = str_suffix_to_u32(val, "max_remove");
			if (intval < 0) {
				rc = intval;
				goto out;
			}
			config->max_remove = intval;
			LOG_INFO("config setting max_remove to %u",
				 config->max_remove);
			continue;
		}
		if (!strcasecmp(key, "hal_size")) {
			long long intval = str_suffix_to_u32(val, "hal_size");
			if (intval < 0) {
				rc = intval;
				goto out;
			}
			config->hsm_action_list_size = intval;
			LOG_INFO("config setting hal_size to %u",
				 config->hsm_action_list_size);
			continue;
		}
		if (!strcasecmp(key, "archive_id")) {
			long long intval = str_suffix_to_u32(val, "archive_id");
			if (intval < 0) {
				rc = intval;
				goto out;
			}
			config->archive_id = intval;
			LOG_INFO("config setting archive_id to %u",
				 config->archive_id);
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

int ct_config_init(struct ct_state_config *config) {
	int rc;

	/* first set defaults */
	config->host = xstrdup("coordinatool");
	config->port = xstrdup("5123");
	config->client_id = NULL; /* NULL will call gethostname if not set */
	config->max_restore = -1;
	config->max_archive = -1;
	config->max_remove = -1;
	config->hsm_action_list_size = 1024 * 1024;
	config->archive_id = 0;
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
	getenv_str("COORDINATOOL_CLIENT_ID", &config->client_id);
	rc = getenv_u32("COORDINATOOL_MAX_RESTORE", &config->max_restore);
	if (rc < 0)
		return rc;
	rc = getenv_u32("COORDINATOOL_MAX_ARCHIVE", &config->max_archive);
	if (rc < 0)
		return rc;
	rc = getenv_u32("COORDINATOOL_MAX_REMOVE", &config->max_remove);
	if (rc < 0)
		return rc;
	rc = getenv_u32("COORDINATOOL_HAL_SIZE", &config->hsm_action_list_size);
	if (rc < 0)
		return rc;
	rc = getenv_u32("COORDINATOOL_ARCHIVE_ID", &config->archive_id);
	if (rc < 0)
		return rc;
	rc = getenv_verbose("COORDINATOOL_VERBOSE", &config->verbose);
	if (rc < 0)
		return rc;

	if (!config->client_id) {
		char client_id[MAXHOSTNAMELEN];
		char *dot;

		rc = gethostname(client_id, sizeof(client_id));
		if (rc) {
			rc = -errno;
			LOG_ERROR(rc, "Could not get hostname!");
			return rc;
		}
		dot = strchr(client_id, '.');
		if (dot)
			*dot = '\0';
		config->client_id = xstrdup(client_id);
	}

	return 0;
}

void ct_config_free(struct ct_state_config *config) {
	free((void*)config->host);
	free((void*)config->port);
	free((void*)config->client_id);
	free((void*)config->state_dir_prefix);
}
