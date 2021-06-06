/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include "client_common.h"

int ct_config_init(struct ct_state_config *config) {
	// XXX magic parameters (conf file + env)
	// for now hardcode
	config->host = "localhost";
	config->port = "5123";
	config->max_restore = -1;
	config->max_archive = -1;
	config->max_remove = -1;
	config->hsm_action_list_size = 1024 * 1024;
	config->archive_id = 0;

	return 0;
}
