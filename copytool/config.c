/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <limits.h>
#include <ctype.h>

#include "coordinatool.h"
#include "utils.h"
#include "config_utils.h"

static const char *SPACES = " \t\n\r\f\v";

static int config_parse_host_mapping(struct cds_list_head *head, char *val) {
	char *data_pattern = strtok(val, SPACES);
	if (!data_pattern) {
		// val is non-empty so should never happen...
		return -EINVAL;
	}
	char *host = strtok(NULL, SPACES);
	if (!host) {
		LOG_INFO("Skipping host pattern for %s with no host",
			 data_pattern);
		return 0;
	}
	struct host_mapping *mapping = xmalloc(sizeof(*mapping) + sizeof(void*));
	mapping->tag = xstrdup(data_pattern);
	mapping->count = 1;
	mapping->hosts[0] = xstrdup(host);
	while ((host = strtok(NULL, SPACES))) {
		mapping->count++;
		mapping = xrealloc(mapping,
				   sizeof(*mapping) + mapping->count * sizeof(void*));
		mapping->hosts[mapping->count - 1] = xstrdup(host);
	}
	cds_list_add(&mapping->node, head);
	return 0;
}

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
		char *key, *val;
		key = strtok(line, SPACES);
		if (key == NULL) /* blank line */
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

		/* rest of line */
		val = strtok(NULL, "");
		if (val) {
			while (isspace(*val)) {
				val++;
			}
		}
		if (val == NULL || *val == '\0') {
			LOG_WARN(rc, "skipping %s in %s (line %zd) not in 'key value' format",
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
				LOG_ERROR(-ERANGE, "%s:%zd: Archive id %s must be > 0",
					  config->confpath, linenum, val);
				rc = EXIT_FAILURE;
				goto out;
			}
			config->archive_cnt++;
			continue;
		}
		if (!strcasecmp(key, "archive_on_hosts")) {
			rc = config_parse_host_mapping(&config->archive_mappings, val);
			if (rc < 0) {
				/* note val has likely been modified and will only contain
				 * the first word, but that's better than nothing. */
				LOG_ERROR(rc, "%s:%zd: Could not parse %s (%s)\n",
					  config->confpath, linenum, key, val);
				rc = EXIT_FAILURE;
				goto out;
			}
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
	config->client_grace_ms = 600000; /* 10 mins */
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

	struct cds_list_head *n, *nnext;
	cds_list_for_each_safe(n, nnext, &config->archive_mappings) {
		struct host_mapping *mapping =
			caa_container_of(n, struct host_mapping, node);
		int i;
		free((void*)mapping->tag);
		for (i = 0; i < mapping->count; i++) {
			free((void*)mapping->hosts[i]);
		}
		free(mapping);
	}
}
