/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "cli.h"
#include "instance.h"
#include "host_config.h"
#include "mount_plan.h"
#include "path_map.h"
#include "executable.h"
#include "backend_nested.h"
#include "doctor.h"
#include "trampoline.h"
#include "mount.h"
#include "package.h"
#include "runtime.h"

#include "package_identity.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CT_EXIT_USAGE 64
#define CT_EXIT_PACKAGE 78

static int ct_internal_compatibility(int argument_count, char **arguments)
{
  if (argument_count < 4 ||
      ct_package_verify_compatibility_identity(arguments[2], arguments[3]) != 0) {
    (void)fputs("container-tools: package verification failed: compatibility identity mismatch\n",
                stderr);
    return CT_EXIT_PACKAGE;
  }
  if (strcmp(arguments[3], "ct_exec.sh") == 0) {
    return ct_runtime_foreground_command(argument_count - 4, arguments + 4, 0);
  }
  if (strcmp(arguments[3], "ct_shell.sh") == 0) {
    return ct_runtime_foreground_command(argument_count - 4, arguments + 4, 1);
  }
  if (strcmp(arguments[3], "ct_instance_exec.sh") == 0) {
    return ct_instance_command(argument_count - 4, arguments + 4, 0);
  }
  if (strcmp(arguments[3], "ct_mount_detector.sh") == 0) {
    return ct_mount_detect_command(argument_count - 4, arguments + 4);
  }
  if (strcmp(arguments[3], "ct_args.sh") == 0) {
    return ct_mount_args_command(argument_count - 4, arguments + 4);
  }
  return CT_EXIT_PACKAGE;
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
  struct ct_process_environment process_environment[CT_HOST_MAX_ENVIRONMENT + 1U];
  char caller_cwd[CT_HOST_PATH_MAX];
  char target_cwd[CT_HOST_PATH_MAX];
};

static int ct_host_process_environment(
    struct ct_host_exec_workspace *workspace, size_t count)
{
  size_t index;
  if (workspace == NULL || count > CT_HOST_MAX_ENVIRONMENT + 1U) return 1;
  for (index = 0U; index < count; ++index) {
    workspace->process_environment[index].name = workspace->environment[index].name;
    workspace->process_environment[index].value =
        workspace->environment[index].kind == CT_HOST_ENVIRONMENT_REMOVE
            ? NULL
            : workspace->environment[index].value;
  }
  return 0;
}

static int ct_host_add_projections(const struct ct_host_profile *profile,
                                   struct ct_path_map *map)
{
  size_t index;
  struct stat status;
  for (index = 0U; index < profile->projection_count; ++index) {
    if (stat(profile->projections[index].visible, &status) != 0) {
      if (profile->projections[index].required != 0 ||
          (errno != ENOENT && errno != ENOTDIR)) {
        return 1;
      }
      continue;
    }
    if (ct_path_map_add(map, profile->projections[index].visible,
                        profile->projections[index].target,
                        profile->projections[index].access, 1U,
                        (unsigned int)index) != 0) {
      return 1;
    }
  }
  return 0;
}

static void ct_host_mount_plan_incompatible(const char *operation,
                                            const struct ct_mount_plan_metadata *metadata)
{
  const char *actual = metadata->grammar[0] == '\0' ? "unknown" : metadata->grammar;

  (void)fprintf(stderr,
                "container-tools: host %s: unsupported mount plan grammar: expected %s, actual %s\n",
                operation, CT_MANIFEST_GRAMMAR, actual);
}

static int ct_host_inherited_descriptor(void)
{
  DIR *directory;
  struct dirent *entry;
  int selected = -1;
  directory = opendir("/proc/self/fd");
  if (directory == NULL) return -1;
  while ((entry = readdir(directory)) != NULL) {
    char *end;
    long descriptor;
    int flags;
    errno = 0;
    descriptor = strtol(entry->d_name, &end, 10);
    if (errno != 0 || end == entry->d_name || *end != '\0' || descriptor < 3 ||
        descriptor > 1048576L) continue;
    flags = fcntl((int)descriptor, F_GETFD);
    if (flags >= 0 && (flags & FD_CLOEXEC) == 0 && descriptor > selected) {
      selected = (int)descriptor;
    }
  }
  (void)closedir(directory);
  return selected;
}

static int ct_host_backend_is_valid(const char *backend)
{
  return backend != NULL &&
         (strcmp(backend, "bubblewrap") == 0 || strcmp(backend, "proot") == 0 ||
          strcmp(backend, "rewrite") == 0);
}

static int ct_host_verbose_selection(const char *operation,
                                     const struct ct_host_profile *profile,
                                     const char *backend)
{
  if (operation == NULL || profile == NULL || backend == NULL) return 1;
  return fprintf(stderr, "container-tools: host %s: selection profile=%.64s backend=%s\n",
                 operation, profile->name, backend) < 0
             ? 1
             : 0;
}

static int ct_host_exec(int argument_count, char **arguments)
{
  const char *config_path = NULL;
  const char *profile_name = NULL;
  const char *backend = NULL;
  int allow_rewrite = 0;
  int verbose = 0;
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
    else if (strcmp(arguments[index], "--backend") == 0) {
      if (backend != NULL || index + 1 >= argument_count ||
          arguments[index + 1][0] == '\0' || strcmp(arguments[index + 1], "--") == 0) {
        ct_path_map_destroy(&map);
        return CT_EXIT_USAGE;
      }
      backend = arguments[++index];
    }
    else if (strcmp(arguments[index], "--allow-degraded=rewrite") == 0 && allow_rewrite == 0) allow_rewrite = 1;
    else if (strcmp(arguments[index], "--verbose") == 0 && verbose == 0) verbose = 1;
    else if (strcmp(arguments[index], "--") == 0) { separator = index; break; }
    else { ct_path_map_destroy(&map); return CT_EXIT_USAGE; }
  }
  if (separator < 0 || separator + 1 >= argument_count ||
      (backend != NULL && ct_host_backend_is_valid(backend) == 0)) {
    ct_path_map_destroy(&map);
    return CT_EXIT_USAGE;
  }
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
  if (verbose != 0 &&
      ct_host_verbose_selection("exec", &workspace->profile,
                                backend == NULL ? "automatic" : backend) != 0) {
    ct_path_map_destroy(&map);
    free(workspace);
    return 125;
  }
  ct_path_map_manifest_init(&manifest, &map, &workspace->profile);
  if (workspace->profile.mount_plan_configured != 0) {
    const enum ct_mount_plan_read_status mount_status = ct_mount_plan_read_status(
        workspace->profile.mount_plan, &workspace->metadata,
        ct_path_map_manifest_entry, &manifest);

    if (mount_status == CT_MOUNT_PLAN_READ_FUTURE) {
      ct_host_mount_plan_incompatible("exec", &workspace->metadata);
      ct_path_map_destroy(&map);
      free(workspace);
      return 125;
    }
    if (mount_status != CT_MOUNT_PLAN_READ_OK ||
        ct_path_map_manifest_eligible(&workspace->profile, &workspace->metadata,
                                      &manifest) != 0) {
      (void)fputs("container-tools: host exec: invalid exact mount plan\n", stderr);
      ct_path_map_destroy(&map);
      free(workspace);
      return 125;
    }
  }
  if (ct_host_add_projections(&workspace->profile, &map) != 0 ||
      ct_path_map_sort(&map) != 0 ||
      getcwd(workspace->caller_cwd, sizeof(workspace->caller_cwd)) == NULL ||
      ct_path_map_cwd(&map, workspace->caller_cwd,
                      workspace->profile.cwd_unmapped,
                      workspace->target_cwd) != 0 ||
      ct_path_map_environment(&workspace->profile, workspace->environment,
                              &environment_count) != 0 ||
      environment_count == 0U ||
      ct_host_process_environment(workspace, environment_count) != 0) {
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
  {
    int trampoline_descriptor;
    int control_descriptor;
    const int inherited_descriptor = ct_host_inherited_descriptor();
    struct ct_nested_request request;
    int dispatch_result;
    trampoline_descriptor = open("/proc/self/exe", O_RDONLY);
    control_descriptor = ct_executable_control_open(CT_BUILD_IDENTITY);
    if (trampoline_descriptor < 0 || control_descriptor < 0) {
      if (trampoline_descriptor >= 0) (void)close(trampoline_descriptor);
      if (control_descriptor >= 0) (void)close(control_descriptor);
      ct_executable_close(&workspace->executable);
      ct_path_map_destroy(&map); free(workspace); return 125;
    }
    arguments[separator + 1] = workspace->executable.target_path;
    request.profile = &workspace->profile; request.map = &map;
    request.executable = &workspace->executable; request.cwd = workspace->target_cwd;
    request.payload = arguments + separator + 1;
    request.environment = workspace->process_environment;
    request.environment_count = environment_count;
    request.trampoline_descriptor = trampoline_descriptor;
    request.control_descriptor = control_descriptor;
    request.inherited_descriptor = inherited_descriptor;
    request.bubblewrap_path = NULL;
    request.proot_path = NULL;
    dispatch_result = ct_backend_nested_execute(&request, backend, allow_rewrite);
    (void)close(trampoline_descriptor); (void)close(control_descriptor);
    ct_executable_close(&workspace->executable);
    ct_path_map_destroy(&map);
    free(workspace);
    return dispatch_result;
  }
}

static const char *ct_host_mount_status(enum ct_mount_plan_read_status status)
{
  switch (status) {
  case CT_MOUNT_PLAN_READ_ABSENT: return "absent";
  case CT_MOUNT_PLAN_READ_MALFORMED: return "malformed";
  case CT_MOUNT_PLAN_READ_FUTURE: return "future";
  case CT_MOUNT_PLAN_READ_DIGEST_MISMATCH: return "digest-mismatch";
  case CT_MOUNT_PLAN_READ_SEMANTIC_INVALID: return "semantic-invalid";
  case CT_MOUNT_PLAN_READ_CHANGED: return "changed-during-read";
  case CT_MOUNT_PLAN_READ_OK:
  case CT_MOUNT_PLAN_READ_IO: return NULL;
  }
  return NULL;
}

static int ct_host_doctor_human(
    const struct ct_host_profile *profile,
    const struct ct_doctor_mount_plan *mount_plan,
    const struct ct_nested_backend_report reports[CT_NESTED_BACKEND_COUNT])
{
  size_t index;
  if (fprintf(stdout, "profile: %s\nmount plan: %s\n", profile->name,
              mount_plan->status) < 0) {
    return 1;
  }
  for (index = 0U; index < CT_NESTED_BACKEND_COUNT; ++index) {
    if (fprintf(stdout, "%s: %s\n", ct_backend_nested_name(reports[index].backend),
                reports[index].reason_code) < 0) {
      return 1;
    }
  }
  return 0;
}

static int ct_host_doctor(int argument_count, char **arguments)
{
  struct ct_host_doctor_workspace {
    struct ct_host_profile profile;
    struct ct_mount_plan_metadata metadata;
  };
  const char *config_path = NULL, *profile_name = NULL;
  int json = 0;
  int verbose = 0;
  int index;
  struct ct_host_doctor_workspace *workspace = NULL;
  struct ct_path_map map;
  struct ct_path_map_manifest_context manifest;
  struct stat status;
  struct ct_nested_request request;
  struct ct_nested_backend_report reports[CT_NESTED_BACKEND_COUNT];
  struct ct_doctor_mount_plan mount_plan;
  enum ct_host_config_status loaded;
  int trampoline_descriptor = -1;
  int control_descriptor = -1;
  int inherited_descriptor = -1;
  int manifest_ready = 1;
  enum ct_mount_plan_read_status manifest_status = CT_MOUNT_PLAN_READ_OK;
  const char *mount_status;
  ct_path_map_init(&map);
  for (index = 0; index < argument_count; ++index) {
    if (strcmp(arguments[index], "--config") == 0 && config_path == NULL && index + 1 < argument_count) config_path = arguments[++index];
    else if (strcmp(arguments[index], "--profile") == 0 && profile_name == NULL && index + 1 < argument_count) profile_name = arguments[++index];
    else if (strcmp(arguments[index], "--json") == 0 && json == 0) json = 1;
    else if (strcmp(arguments[index], "--verbose") == 0 && verbose == 0) verbose = 1;
    else { ct_path_map_destroy(&map); return 64; }
  }
  workspace = calloc(1U, sizeof(*workspace));
  if (workspace == NULL) goto invalid;
  memset(&mount_plan, 0, sizeof(mount_plan));
  loaded = ct_host_config_load(config_path, profile_name, &workspace->profile);
  if (loaded == CT_HOST_CONFIG_ABSENT && config_path == NULL && profile_name == NULL) { ct_host_default_profile(&workspace->profile); loaded = CT_HOST_CONFIG_OK; }
  if (loaded != CT_HOST_CONFIG_OK || stat(workspace->profile.root, &status) != 0 || !S_ISDIR(status.st_mode) || ct_path_map_set_root(&map, workspace->profile.root) != 0) goto invalid;
  if (verbose != 0 &&
      ct_host_verbose_selection("doctor", &workspace->profile, "bubblewrap,proot,rewrite") != 0) {
    goto invalid;
  }
  mount_plan.configured = workspace->profile.mount_plan_configured;
  mount_plan.status = workspace->profile.mount_plan_configured != 0 ? "ready" : "disabled";
  mount_plan.detail = "";
  ct_path_map_manifest_init(&manifest, &map, &workspace->profile);
  if (workspace->profile.mount_plan_configured != 0) {
    manifest_status = ct_mount_plan_read_status(
        workspace->profile.mount_plan, &workspace->metadata,
        ct_path_map_manifest_entry, &manifest);
    if (manifest_status != CT_MOUNT_PLAN_READ_OK) {
      if (manifest_status == CT_MOUNT_PLAN_READ_FUTURE) {
        ct_host_mount_plan_incompatible("doctor", &workspace->metadata);
        goto invalid;
      }
      mount_status = ct_host_mount_status(manifest_status);
      if (mount_status == NULL) goto invalid;
      mount_plan.status = mount_status;
      mount_plan.detail = "exact mount plan was not usable";
      manifest_ready = 0;
    } else {
      mount_plan.digest = workspace->metadata.digest;
      mount_plan.strategy = workspace->metadata.strategy;
      mount_plan.completeness = workspace->metadata.completeness;
      if (strcmp(workspace->metadata.completeness, "partial") == 0) {
        mount_plan.status = "partial";
        manifest_ready = 0;
      } else if (strcmp(workspace->metadata.strategy, "none") == 0) {
        mount_plan.status = "none";
        manifest_ready = 0;
      } else if (ct_path_map_manifest_eligible(&workspace->profile,
                                                &workspace->metadata,
                                                &manifest) != 0) {
        mount_plan.status = "semantic-invalid";
        mount_plan.detail = "mount plan is incompatible with the selected profile";
        manifest_ready = 0;
      }
    }
  }
  if (manifest_ready != 0 &&
      (ct_host_add_projections(&workspace->profile, &map) != 0 ||
       ct_path_map_sort(&map) != 0)) goto invalid;
  inherited_descriptor = ct_host_inherited_descriptor();
  trampoline_descriptor = open("/proc/self/exe", O_RDONLY);
  control_descriptor = ct_executable_control_open(CT_BUILD_IDENTITY);
  if (trampoline_descriptor < 0 || control_descriptor < 0) goto invalid;
  memset(&request, 0, sizeof(request));
  request.profile = &workspace->profile; request.map = &map; request.executable = NULL;
  request.cwd = "/"; request.payload = NULL; request.environment = NULL;
  request.environment_count = 0U;
  request.trampoline_descriptor = trampoline_descriptor;
  request.control_descriptor = control_descriptor;
  request.inherited_descriptor = inherited_descriptor;
  request.bubblewrap_path = NULL; request.proot_path = NULL;
  if ((manifest_ready != 0
           ? ct_backend_nested_diagnose(&request, 0, reports)
           : ct_backend_nested_blocked(&request, "mount-plan-unavailable", reports)) != 0 ||
      (json != 0 ? ct_doctor_json(stdout, &workspace->profile, &mount_plan, reports)
                 : ct_host_doctor_human(&workspace->profile, &mount_plan, reports)) != 0) {
    goto invalid;
  }
  (void)close(trampoline_descriptor);
  (void)close(control_descriptor);
  ct_path_map_destroy(&map);
  free(workspace);
  return 0;
invalid:
  if (trampoline_descriptor >= 0) (void)close(trampoline_descriptor);
  if (control_descriptor >= 0) (void)close(control_descriptor);
  ct_path_map_destroy(&map);
  free(workspace);
  (void)fputs("container-tools: host doctor: unable to produce complete report\n", stderr);
  return 125;
}

int main(int argument_count, char **arguments)
{
  struct ct_cli parsed;

  if (argument_count >= 2 && strcmp(arguments[1], "--internal-compat") == 0) {
    return ct_internal_compatibility(argument_count, arguments);
  }
  if (argument_count >= 2 && strcmp(arguments[1], "--internal-trampoline") == 0) {
    return ct_trampoline_main(argument_count - 1, arguments + 1, CT_BUILD_IDENTITY);
  }
  if (ct_cli_parse(argument_count - 1, (const char *const *)(arguments + 1), &parsed) !=
      CT_CLI_OK) {
    ct_cli_write_usage(stderr);
    return CT_EXIT_USAGE;
  }
  if (parsed.command == CT_COMMAND_HELP) {
    ct_cli_write_usage(stdout);
    return 0;
  }
  if (parsed.command == CT_COMMAND_VERSION) {
    if (parsed.json != 0) {
      (void)fprintf(stdout, "%s\n", ct_package_version_json());
    } else {
      (void)fprintf(stdout, "container-tools %s commit=%s architecture=%s mount-plan=%s\n",
                    CT_PRODUCT_VERSION, CT_SOURCE_COMMIT, CT_ARCHITECTURE,
                    CT_MANIFEST_GRAMMAR);
    }
    return 0;
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
  if (parsed.command == CT_COMMAND_HOST_DOCTOR) {
    return ct_host_doctor(argument_count - 3, arguments + 3);
  }
  return CT_EXIT_USAGE;
}
