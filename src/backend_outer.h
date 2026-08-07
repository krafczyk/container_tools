/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_BACKEND_OUTER_H
#define CONTAINER_TOOLS_BACKEND_OUTER_H

#include <stddef.h>

/** Run an already-rendered outer-runtime argv exactly once under process ownership. */
int ct_backend_outer_dispatch(char *const arguments[], size_t count);

/**
 * Render one backend-specific non-recursive host-projection mount argument.
 *
 * `source` and `target` are validated absolute paths supplied by the caller.
 * On success, `output` receives the mount descriptor and `flag` identifies the
 * preceding argv token. Returns nonzero for an unsupported backend, invalid
 * output storage, or truncation.
 */
int ct_backend_outer_projection_mount(const char *backend, const char *source,
                                      const char *target, char *output,
                                      size_t output_size, const char **flag);

#endif
