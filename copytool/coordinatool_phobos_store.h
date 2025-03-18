/* SPDX-License-Identifier: LGPL-3.0-or-later */

#pragma once

/* including phobos_store.h directly has a couple of problems:
 * - it's pulling in glib.h which conflicts with lustreapi without the
 *   fallthrough workaround below
 *   see https://jira.whamcloud.com/browse/LU-18631
 * - it's defining it's own xmalloc/xstrdup/etc which we don't want */

#undef fallthrough

#include <phobos_store.h>

#undef xmalloc
#undef xcalloc
#undef xstrdup
#undef xrealloc
