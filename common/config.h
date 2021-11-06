/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef COORDINATOOL_CONFIG_H
#define COORDINATOOL_CONFIG_H

#include "lustre.h"

int getenv_str(const char *name, const char **val);
int getenv_u32(const char *name, uint32_t *val);
int getenv_verbose(const char *name, enum llapi_message_level *val);
enum llapi_message_level str_to_verbose(const char *str);
long long str_suffix_to_u32(const char *str, const char *error_hint);

#endif
