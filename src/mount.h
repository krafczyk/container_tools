/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_MOUNT_H
#define CONTAINER_TOOLS_MOUNT_H

#include <stddef.h>

#define CT_MOUNT_MAX_PATHS 1024U
#define CT_MOUNT_PATH_MAX 4096U

/** Result codes returned by bounded mount argument and detector operations. */
enum ct_mount_status {
  CT_MOUNT_OK = 0,
  CT_MOUNT_INVALID = 1,
  CT_MOUNT_IO = 2,
  CT_MOUNT_NOT_FOUND = 3
};

/**
 * Format legacy whitespace-tokenized mount arguments while preserving flag groups.
 *
 * @param file_tokens Tokens read from the configuration file.
 * @param file_count Number of file tokens.
 * @param extra_tokens Caller argv tokens.
 * @param extra_count Number of caller tokens.
 * @param output Destination presentation string.
 * @param output_size Destination capacity.
 * @return CT_MOUNT_OK or a bounded failure status.
 */
enum ct_mount_status ct_mount_format_args(const char *const *file_tokens,
                                          size_t file_count,
                                          const char *const *extra_tokens,
                                          size_t extra_count, char *output,
                                          size_t output_size);

/** Execute the public `mount args` operation. */
int ct_mount_args_command(int argument_count, char *const arguments[]);

/** Execute the public `mount detect` operation. */
int ct_mount_detect_command(int argument_count, char *const arguments[]);

/**
 * Collect the mount paths selected by the process mount configuration.
 *
 * Reads `CT_MOUNT_CFG` or `$HOME/.config/ct_mount.conf`, merges the optional
 * whitespace-tokenized `MOUNT_DETECTOR_ARGS`, and applies the same detector
 * ordering and filtering as `mount detect`. A missing configuration file is
 * equivalent to an empty one. Returns nonzero without partial results on
 * malformed options, unavailable configuration, or detector overflow.
 */
int ct_mount_collect_environment(
    char paths[CT_MOUNT_MAX_PATHS][CT_MOUNT_PATH_MAX], size_t *path_count);

#endif
