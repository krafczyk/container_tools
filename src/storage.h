/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_STORAGE_H
#define CONTAINER_TOOLS_STORAGE_H

#include "config.h"

/**
 * Create or validate one private runtime directory without following symlinks.
 *
 * @param path Absolute normalized path to create or adopt.
 * @return Zero when the directory is current-user private, otherwise nonzero.
 */
int ct_storage_ensure_private_directory(const char *path);

/**
 * Select native runtime storage for one child environment.
 *
 * Explicit native environment variables win over configuration defaults.
 *
 * @param backend singularity or apptainer.
 * @param config Parsed machine configuration.
 * @return Zero on success, otherwise nonzero before child dispatch.
 */
int ct_storage_select_runtime(const char *backend, const struct ct_runtime_config *config);

#endif
