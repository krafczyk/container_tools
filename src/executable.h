/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_EXECUTABLE_H
#define CONTAINER_TOOLS_EXECUTABLE_H

#include "elf.h"
#include "host_config.h"
#include "path_map.h"
#include "shebang.h"

#define CT_EXECUTABLE_MAX_STAGES 5U
#define CT_EXECUTABLE_ENV_ARGUMENT_MAX 16U

/** Typed result from target-side executable resolution. */
enum ct_executable_status {
  CT_EXECUTABLE_OK = 0,
  CT_EXECUTABLE_NOT_FOUND = 1,
  CT_EXECUTABLE_INCOMPATIBLE = 2,
  CT_EXECUTABLE_IO = 3
};

/** One stable inspected ELF or shebang stage in target-side execution order. */
struct ct_executable_stage {
  char target_path[CT_HOST_PATH_MAX];
  char visible_path[CT_HOST_PATH_MAX];
  struct ct_elf_info elf;
  struct ct_shebang shebang;
  char env_arguments[CT_EXECUTABLE_ENV_ARGUMENT_MAX][256];
  size_t env_argument_count;
  int descriptor;
  int is_shebang;
};

/** Resolved command, stable execution stages, and optional dynamic loader facts. */
struct ct_executable {
  char target_path[CT_HOST_PATH_MAX];
  char visible_path[CT_HOST_PATH_MAX];
  int descriptor;
  char loader_target_path[CT_HOST_PATH_MAX];
  char loader_visible_path[CT_HOST_PATH_MAX];
  int loader_descriptor;
  struct ct_executable_stage stages[CT_EXECUTABLE_MAX_STAGES];
  size_t stage_count;
};

/**
 * Resolve and inspect one target-side command using the profile's target PATH.
 *
 * Follows ordinary caller-visible symlinks and checks caller execute access.
 * Absolute shebang and GNU env entry chains are inspected through a maximum of
 * four script interpreters. Every inspected stage and the dynamic ELF loader,
 * when present, retain a stable descriptor on success. GNU `env -S` command
 * arguments after the resolved command are normalized into the relevant stage.
 * `ct_executable_close` closes every retained descriptor.
 *
 * @param profile Valid selected-root profile.
 * @param map Complete composed selected-root path map.
 * @param command Bare PATH command or absolute target-side path.
 * @param executable Destination resolved chain.
 * @return Typed resolution result; no descriptor remains open on failure.
 */
enum ct_executable_status ct_executable_resolve(
    const struct ct_host_profile *profile, const struct ct_path_map *map,
    const char *command,
    struct ct_executable *executable);
/** Close all owned entry-stage and loader descriptors, if any, and mark them closed. */
void ct_executable_close(struct ct_executable *executable);
/**
 * Admit the current static executable and a bounded internal control record.
 *
 * The control descriptor must contain `container-tools-control-v1\n` followed
 * by the exact embedded build identity and a trailing newline. Both descriptors
 * are fstat-checked before and after reading to reject changed files.
 *
 * @param executable_descriptor Open descriptor for the current static executable.
 * @param control_descriptor Open bounded control-record descriptor.
 * @param build_identity Exact expected package build identity.
 * The executable descriptor must identify `/proc/self/exe` and that executable
 * must be a native static ELF. Neither descriptor is closed by this function.
 *
 * @return Zero only when both stable descriptors and the record match.
 */
int ct_executable_admit_trampoline(int executable_descriptor, int control_descriptor, const char *build_identity);
/**
 * Create a non-close-on-exec bounded control record for trampoline admission.
 *
 * @param build_identity Exact package build identity written into the bounded
 *        `container-tools-control-v1` record.
 * @return Open descriptor positioned at offset zero, or -1 on failure.
 */
int ct_executable_control_open(const char *build_identity);

#endif
