/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_CONFIG_H
#define CONTAINER_TOOLS_CONFIG_H

#include <stddef.h>

#define CT_CONFIG_PATH_MAX 4096U

/** Result codes returned while reading the machine-local runtime configuration. */
enum ct_config_status { CT_CONFIG_OK = 0, CT_CONFIG_INVALID = 1, CT_CONFIG_IO = 2 };

/** Bounded runtime-storage defaults parsed from ct_runtime.conf. */
struct ct_runtime_config {
  char singularity_cache_dir[CT_CONFIG_PATH_MAX];
  char singularity_tmp_dir[CT_CONFIG_PATH_MAX];
};

/**
 * Parse ct_runtime.conf bytes without evaluating them as shell input.
 *
 * @param contents NUL-terminated, bounded config text without embedded NUL bytes.
 * @param config Destination initialized only when all records are valid.
 * @return CT_CONFIG_OK, CT_CONFIG_INVALID for malformed records, or CT_CONFIG_IO.
 */
enum ct_config_status ct_runtime_config_parse(const char *contents,
                                              struct ct_runtime_config *config);

/**
 * Read the configured file after validating its descriptor ownership, mode, and bytes.
 *
 * @param path Optional absolute configuration path; an absent file yields defaults.
 * @param config Destination parsed only after complete validation.
 * @return CT_CONFIG_OK, CT_CONFIG_INVALID, or CT_CONFIG_IO.
 */
enum ct_config_status ct_runtime_config_load(const char *path,
                                             struct ct_runtime_config *config);

/**
 * Resolve CT_RUNTIME_CFG or HOME/.config/ct_runtime.conf and load its defaults.
 *
 * @param config Destination parsed only after complete validation.
 * @return CT_CONFIG_OK, CT_CONFIG_INVALID, or CT_CONFIG_IO.
 */
enum ct_config_status ct_runtime_config_load_environment(struct ct_runtime_config *config);

#endif
