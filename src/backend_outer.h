/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_BACKEND_OUTER_H
#define CONTAINER_TOOLS_BACKEND_OUTER_H

#include <stddef.h>

/** Run an already-rendered outer-runtime argv exactly once under process ownership. */
int ct_backend_outer_dispatch(char *const arguments[], size_t count);

#endif
