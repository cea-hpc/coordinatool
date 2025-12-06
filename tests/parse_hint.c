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

int parse_one_item(json_t *json)
{
	const char *data, *needle, *match, *hint;
	size_t data_len, match_len, hint_len;
	data = protocol_getjson_str(json, "data", NULL, &data_len);
	needle = protocol_getjson_str(json, "needle", NULL, NULL);
	match = protocol_getjson_str(json, "match", NULL, &match_len);

	if (!data) {
		fprintf(stderr, "invalid test data\n");
		return 1;
	}

	struct hsm_action_node han = {
		.info.data = data,
		.info.hai_len = data_len + sizeof(struct hsm_action_item),
	};
	hint = parse_hint(&han, needle, &hint_len);
	if (!match && !hint)
		return 0;
	if (!hint) {
		fprintf(stderr, "no hint found\n");
		return 1;
	}
	if (!match) {
		fprintf(stderr, "found hint \"%s\" when expecting none\n",
			hint);
		return 1;
	}
	if (hint_len != match_len || strncmp(hint, match, hint_len)) {
		fprintf(stderr, "hint mismatch: \"%s\" (%zd) != \"%s\" (%zd)\n",
			hint, hint_len, match, match_len);
		return 1;
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
		rc = parse_one_item(json);
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
