/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_HOST_CONFIG_H
#define CONTAINER_TOOLS_HOST_CONFIG_H

#include <stddef.h>

#define CT_HOST_PATH_MAX 4096U
#define CT_HOST_MAX_PROJECTIONS 256U
#define CT_HOST_MAX_PATHS 256U
#define CT_HOST_MAX_ENVIRONMENT 128U

/** Closed outcomes from selected-root profile discovery and parsing. */
enum ct_host_config_status { CT_HOST_CONFIG_OK = 0, CT_HOST_CONFIG_ABSENT = 1, CT_HOST_CONFIG_INVALID = 2, CT_HOST_CONFIG_IO = 3 };

/** One configured caller-visible projection into the selected root. */
struct ct_host_config_projection {
  char visible[CT_HOST_PATH_MAX];
  char target[CT_HOST_PATH_MAX];
  char access[16];
  int required;
};

/** One environment change applied after inherited-variable removals. */
struct ct_host_environment { char name[128]; char value[CT_HOST_PATH_MAX]; };

/** Bounded strict TOML selected-root profile. */
struct ct_host_profile {
  char name[128];
  char root[CT_HOST_PATH_MAX];
  char root_access[16];
  char semantics[16];
  char mount_plan[CT_HOST_PATH_MAX];
  int mount_plan_configured;
  char cwd_unmapped[16];
  char path[CT_HOST_MAX_PATHS][CT_HOST_PATH_MAX];
  size_t path_count;
  char environment_remove[CT_HOST_MAX_ENVIRONMENT][128];
  size_t environment_remove_count;
  struct ct_host_environment environment[CT_HOST_MAX_ENVIRONMENT];
  size_t environment_count;
  struct ct_host_config_projection projections[CT_HOST_MAX_PROJECTIONS];
  size_t projection_count;
};

/**
 * Parse one bounded strict host profile TOML document.
 *
 * The document uses the container-tools namespace and accepts only version,
 * default_profile, and profiles at top level. `selected_profile` may be NULL
 * to select default_profile. The destination is written only on success.
 *
 * @param contents TOML bytes followed by one caller-owned NUL byte.
 * @param length Exact TOML byte count, no larger than one MiB.
 * @param selected_profile Optional explicit profile name.
 * @param profile Destination for the selected closed profile.
 * @return A typed CT_HOST_CONFIG_* result without probing a backend.
 */
enum ct_host_config_status ct_host_config_parse(const char *contents,
                                                size_t length,
                                                const char *selected_profile,
                                                struct ct_host_profile *profile);

/**
 * Discover and load a selected-root profile without consulting the current directory.
 *
 * Discovery is explicit path, CONTAINER_TOOLS_HOST_CONFIG, XDG_CONFIG_HOME/container-tools/host.toml,
 * then HOME/.config/container-tools/host.toml. An explicit missing path is an IO
 * error; absent implicit candidates return CT_HOST_CONFIG_ABSENT.
 *
 * @param explicit_path Optional explicit absolute profile file.
 * @param selected_profile Optional profile override.
 * @param profile Destination for the selected profile.
 * @return A typed CT_HOST_CONFIG_* result.
 */
enum ct_host_config_status ct_host_config_load(const char *explicit_path, const char *selected_profile, struct ct_host_profile *profile);

/** Return zero only for one normalized absolute host-planning path. */
int ct_host_path_validate(const char *value);

/** Copy one NUL-terminated value into a bounded destination. */
int ct_host_copy_bounded(char *destination, size_t size, const char *source);

#endif
