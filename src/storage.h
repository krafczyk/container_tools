/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_STORAGE_H
#define CONTAINER_TOOLS_STORAGE_H

#include "config.h"

/**
 * Create or validate one real runtime directory without following symlinks.
 * New directories request mode 0700; reported ownership and permission bits
 * are not admission criteria for existing directories.
 *
 * @param path Absolute normalized path to create or adopt.
 * @return Zero when every existing component is a real directory, otherwise nonzero.
 * @sideeffect May create path and missing real-directory ancestors.
 */
int ct_storage_ensure_directory(const char *path);

/**
 * Select native runtime storage for one child environment.
 *
 * Explicit native environment variables win over configuration defaults.
 *
 * @param backend singularity or apptainer.
 * @param config Parsed machine configuration.
 * @return Zero on success, otherwise nonzero before child dispatch.
 * @sideeffect May create configured directories and set the selected native
 *             runtime environment variables. Writes one redacted actionable
 *             diagnostic to stderr on failure.
 */
int ct_storage_select_runtime(const char *backend, const struct ct_runtime_config *config);

#endif
