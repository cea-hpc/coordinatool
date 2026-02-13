/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "protocol.h"

/* We cheat around the build system a bit here as there is no "libcopytool" to
 * reuse this utils.c, and we can't include copytool.h without a config.h
 * Ideally we should define hsm_action_node in common.h or update parse_hint
 * to avoid using it, and move the function to common code... */
#define NO_CONFIG_H 1
#include "../copytool/utils.c"

int replace_one_string(json_t *json)
{
	const char *orig, *needle, *old_value, *new_value, *match;
	size_t orig_len, old_len, new_len, match_len;
	size_t data_len;
	char *data;

	orig = protocol_getjson_str(json, "data", NULL, &orig_len);
	needle = protocol_getjson_str(json, "needle", NULL, NULL);
	new_value = protocol_getjson_str(json, "value", NULL, &new_len);
	match = protocol_getjson_str(json, "match", NULL, &match_len);

	if (!orig) {
		fprintf(stderr, "invalid test data\n");
		return 1;
	}

	struct hsm_action_node han = {
		.info.data = orig,
		.info.hai_len = orig_len + sizeof(struct hsm_action_item),
	};
	old_value = parse_hint(&han, needle, &old_len);

	data = replace_string(orig, orig_len, new_value, new_len, old_value,
                          old_len);
	data_len = strlen(data);

	if (data_len != match_len || strncmp(data, match, data_len)) {
		fprintf(stderr,
			"string mismatch: \"%s\" (%zd) != \"%s\" (%zd)\n",
			data, data_len, match, match_len);
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int rc = 0;
	int fd = 0;

	if (argc == 2) {
		fd = open(argv[1], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "Could not open %s: %s\n", argv[1],
				strerror(errno));
			return 1;
		}
	}

	char *line = NULL;
	size_t linelen = 0;
	FILE *file = fdopen(fd, "r");
	ssize_t len;

	assert(file);
	while ((len = getline(&line, &linelen, file)) > 0) {
		json_error_t error;
		json_t *json = json_loadb(line, len, 0, &error);
		if (!json)
			break;
		rc = replace_one_string(json);
		json_decref(json);
		if (rc) {
			// (line contains new line character)
			fprintf(stderr, "Failed %s", line);
			break;
		}
	}
	free(line);
	fclose(file);

	return rc;
}
