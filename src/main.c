/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "cli.h"
#include "buildx.h"
#include "config.h"
#include "instance.h"
#include "host_config.h"
#include "mount_plan.h"
#include "path_map.h"
#include "executable.h"
#include "mount.h"
#include "package.h"
#include "process.h"
#include "storage.h"
#include "runtime.h"

#include "package_identity.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CT_EXIT_USAGE 64
#define CT_EXIT_NOT_IMPLEMENTED 70
#define CT_EXIT_PACKAGE 78

static const char *ct_command_name(enum ct_command command)
{
  switch (command) {
  case CT_COMMAND_EXEC: return "exec";
  case CT_COMMAND_SHELL: return "shell";
  case CT_COMMAND_INSTANCE_EXEC: return "instance exec";
  case CT_COMMAND_INSTANCE_IDENTITY: return "instance identity";
  case CT_COMMAND_MOUNT_DETECT: return "mount detect";
  case CT_COMMAND_MOUNT_ARGS: return "mount args";
  case CT_COMMAND_RUNTIME_EXEC: return "runtime exec";
  case CT_COMMAND_BUILDX_EXEC: return "buildx exec";
  case CT_COMMAND_HOST_EXEC: return "host exec";
  case CT_COMMAND_HOST_DOCTOR: return "host doctor";
  case CT_COMMAND_HELP:
  case CT_COMMAND_VERSION:
  case CT_COMMAND_PACKAGE_VERIFY: return "package verify";
  }
  return "unknown";
}

static int ct_internal_compatibility(int argument_count, char **arguments)
{
  if (argument_count < 4 ||
      ct_package_verify_compatibility_identity(arguments[2], arguments[3]) != 0) {
    (void)fputs("container-tools: package verification failed: compatibility identity mismatch\n",
                stderr);
    return CT_EXIT_PACKAGE;
  }
  if (ct_package_validate(stderr) != 0) {
    return CT_EXIT_PACKAGE;
  }
  (void)fprintf(stderr, "container-tools: not implemented: compat %s\n", arguments[3]);
  return CT_EXIT_NOT_IMPLEMENTED;
}

static int ct_runtime_exec(int argument_count, char **arguments)
{
  struct ct_runtime_config config;
  const char *backend = NULL;
  int separator = -1;
  int index;

  for (index = 0; index < argument_count; ++index) {
    if (strcmp(arguments[index], "--backend") == 0 && backend == NULL &&
        index + 1 < argument_count) {
      backend = arguments[++index];
    } else if (strcmp(arguments[index], "--") == 0) {
      separator = index;
      break;
    } else {
      return CT_EXIT_USAGE;
    }
  }
  if (backend == NULL || separator < 0 || separator + 1 >= argument_count ||
      ct_runtime_config_load_environment(&config) != CT_CONFIG_OK ||
      ct_storage_select_runtime(backend, &config) != 0) {
    (void)fputs("container-tools: runtime exec: invalid storage configuration\n", stderr);
    return 1;
  }
  return ct_process_run(arguments + separator + 1, NULL, 0U);
}

static void ct_host_default_profile(struct ct_host_profile *profile)
{
  (void)snprintf(profile->name, sizeof(profile->name), "%s", "host");
  (void)snprintf(profile->root, sizeof(profile->root), "%s", "/host");
  (void)snprintf(profile->root_access, sizeof(profile->root_access), "%s", "inherit");
  (void)snprintf(profile->semantics, sizeof(profile->semantics), "%s", "full-root");
  (void)snprintf(profile->mount_plan, sizeof(profile->mount_plan), "%s", "/.container-tools-mount-plan");
  (void)snprintf(profile->cwd_unmapped, sizeof(profile->cwd_unmapped), "%s", "error");
  (void)snprintf(profile->path[0], sizeof(profile->path[0]), "%s", "/usr/local/bin");
  (void)snprintf(profile->path[1], sizeof(profile->path[1]), "%s", "/usr/bin");
  (void)snprintf(profile->path[2], sizeof(profile->path[2]), "%s", "/bin");
  profile->mount_plan_configured = 1;
  profile->path_count = 3U;
  profile->environment_remove_count = 0U;
  profile->environment_count = 0U;
  profile->projection_count = 0U;
}

struct ct_host_exec_workspace {
  struct ct_host_profile profile;
  struct ct_mount_plan_metadata metadata;
  struct ct_executable executable;
  struct ct_host_environment_operation environment[CT_HOST_MAX_ENVIRONMENT + 1U];
  char caller_cwd[CT_HOST_PATH_MAX];
  char target_cwd[CT_HOST_PATH_MAX];
};

static int ct_host_exec(int argument_count, char **arguments)
{
  const char *config_path = NULL;
  const char *profile_name = NULL;
  int separator = -1;
  int index;
  struct ct_host_exec_workspace *workspace;
  struct ct_path_map map;
  struct ct_path_map_manifest_context manifest;
  struct stat status;
  size_t environment_count;
  enum ct_host_config_status loaded;
  enum ct_executable_status executable_result;
  ct_path_map_init(&map);
  for (index = 0; index < argument_count; ++index) {
    if (strcmp(arguments[index], "--config") == 0 && config_path == NULL && index + 1 < argument_count) config_path = arguments[++index];
    else if (strcmp(arguments[index], "--profile") == 0 && profile_name == NULL && index + 1 < argument_count) profile_name = arguments[++index];
    else if (strcmp(arguments[index], "--") == 0) { separator = index; break; }
    else { ct_path_map_destroy(&map); return CT_EXIT_USAGE; }
  }
  if (separator < 0 || separator + 1 >= argument_count) { ct_path_map_destroy(&map); return CT_EXIT_USAGE; }
  workspace = calloc(1U, sizeof(*workspace));
  if (workspace == NULL) return 125;
  loaded = ct_host_config_load(config_path, profile_name, &workspace->profile);
  if (loaded == CT_HOST_CONFIG_ABSENT && config_path == NULL && profile_name == NULL) { ct_host_default_profile(&workspace->profile); loaded = CT_HOST_CONFIG_OK; }
  if (loaded != CT_HOST_CONFIG_OK || stat(workspace->profile.root, &status) != 0 ||
      !S_ISDIR(status.st_mode) || ct_path_map_set_root(&map, workspace->profile.root) != 0) {
    (void)fputs("container-tools: host exec: invalid selected-root profile\n", stderr);
    ct_path_map_destroy(&map);
    free(workspace);
    return 125;
  }
  ct_path_map_manifest_init(&manifest, &map, &workspace->profile);
  if (workspace->profile.mount_plan_configured != 0 &&
      (ct_mount_plan_read(workspace->profile.mount_plan, &workspace->metadata,
                          ct_path_map_manifest_entry, &manifest) != 0 ||
       ct_path_map_manifest_eligible(&workspace->profile, &workspace->metadata,
                                     &manifest) != 0)) {
    (void)fputs("container-tools: host exec: invalid exact mount plan\n", stderr);
    ct_path_map_destroy(&map);
    free(workspace);
    return 125;
  }
  for (index = 0; index < (int)workspace->profile.projection_count; ++index) {
    if (stat(workspace->profile.projections[index].visible, &status) != 0) {
      if (workspace->profile.projections[index].required != 0 ||
          (errno != ENOENT && errno != ENOTDIR)) {
        ct_path_map_destroy(&map);
        free(workspace);
        return 125;
      }
      continue;
    }
    if (ct_path_map_add(&map, workspace->profile.projections[index].visible,
                        workspace->profile.projections[index].target,
                        workspace->profile.projections[index].access, 1U,
                        (unsigned int)index) != 0) {
      ct_path_map_destroy(&map);
      free(workspace);
      return 125;
    }
  }
  if (ct_path_map_sort(&map) != 0 ||
      getcwd(workspace->caller_cwd, sizeof(workspace->caller_cwd)) == NULL ||
      ct_path_map_cwd(&map, workspace->caller_cwd,
                      workspace->profile.cwd_unmapped,
                      workspace->target_cwd) != 0 ||
      ct_path_map_environment(&workspace->profile, workspace->environment,
                              &environment_count) != 0 ||
      environment_count == 0U) {
    (void)fputs("container-tools: host exec: command planning failed\n", stderr);
    ct_path_map_destroy(&map);
    free(workspace);
    return 126;
  }
  executable_result = ct_executable_resolve(
      &workspace->profile, &map, arguments[separator + 1],
      &workspace->executable);
  if (executable_result != CT_EXECUTABLE_OK) {
    (void)fputs(executable_result == CT_EXECUTABLE_NOT_FOUND
                    ? "container-tools: host exec: command not found\n"
                    : "container-tools: host exec: command is not compatible\n",
                 stderr);
    ct_path_map_destroy(&map);
    free(workspace);
    return executable_result == CT_EXECUTABLE_NOT_FOUND ? 127 : 126;
  }
  ct_executable_close(&workspace->executable);
  ct_path_map_destroy(&map);
  free(workspace);
  (void)fputs("container-tools: host exec: nested backend unavailable\n", stderr);
  return 125;
}

int main(int argument_count, char **arguments)
{
  struct ct_cli parsed;

  if (argument_count >= 2 && strcmp(arguments[1], "--internal-compat") == 0) {
    return ct_internal_compatibility(argument_count, arguments);
  }
  if (ct_cli_parse(argument_count - 1, (const char *const *)(arguments + 1), &parsed) !=
      CT_CLI_OK) {
    ct_cli_write_usage(stderr);
    return CT_EXIT_USAGE;
  }
  if (parsed.command == CT_COMMAND_PACKAGE_VERIFY) {
    return ct_package_verify(stdout, parsed.json != 0);
  }
  if (ct_package_validate(stderr) != 0) {
    return CT_EXIT_PACKAGE;
  }
  if (parsed.command == CT_COMMAND_HELP) {
    ct_cli_write_usage(stdout);
    return 0;
  }
  if (parsed.command == CT_COMMAND_VERSION) {
    if (parsed.json != 0) {
      (void)fprintf(stdout, "%s\n", ct_package_release_json());
    } else {
      (void)fprintf(stdout, "container-tools %s (%s)\n", CT_PRODUCT_VERSION,
                    CT_BUILD_IDENTITY);
    }
    return 0;
  }
  if (parsed.command == CT_COMMAND_RUNTIME_EXEC) {
    return ct_runtime_exec(argument_count - 3, arguments + 3);
  }
  if (parsed.command == CT_COMMAND_BUILDX_EXEC) {
    return ct_buildx_exec_command(argument_count - 3, arguments + 3);
  }
  if (parsed.command == CT_COMMAND_MOUNT_ARGS) {
    return ct_mount_args_command(argument_count - 3, arguments + 3);
  }
  if (parsed.command == CT_COMMAND_MOUNT_DETECT) {
    return ct_mount_detect_command(argument_count - 3, arguments + 3);
  }
  if (parsed.command == CT_COMMAND_EXEC) {
    return ct_runtime_foreground_command(argument_count - 2, arguments + 2, 0);
  }
  if (parsed.command == CT_COMMAND_SHELL) {
    return ct_runtime_foreground_command(argument_count - 2, arguments + 2, 1);
  }
  if (parsed.command == CT_COMMAND_INSTANCE_EXEC) {
    return ct_instance_command(argument_count - 3, arguments + 3, 0);
  }
  if (parsed.command == CT_COMMAND_INSTANCE_IDENTITY) {
    return ct_instance_command(argument_count - 3, arguments + 3, 1);
  }
  if (parsed.command == CT_COMMAND_HOST_EXEC) {
    return ct_host_exec(argument_count - 3, arguments + 3);
  }
  (void)fprintf(stderr, "container-tools: not implemented: %s\n",
                ct_command_name(parsed.command));
  return CT_EXIT_NOT_IMPLEMENTED;
}
