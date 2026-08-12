/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "cli.h"
#include "instance.h"
#include "host_config.h"
#include "mount_plan.h"
#include "mount_plan_report.h"
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
#include <signal.h>
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
    ct_cli_diagnostic("compatibility launcher", "package-identity",
                      "reinstall the compatibility scripts and container-tools executable from the same package, then retry");
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
  ct_cli_diagnostic("compatibility launcher", "unsupported-script",
                    "reinstall the supported compatibility scripts from the container-tools package, then retry");
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

static void ct_host_usage(const char *operation)
{
  ct_cli_diagnostic(
      operation, "usage",
      strcmp(operation, "host exec") == 0
          ? "use 'container-tools host exec [--config PATH] [--profile NAME] [--backend bubblewrap|proot|rewrite] [--allow-degraded=rewrite] [--verbose] -- COMMAND [ARG...]'"
          : "use 'container-tools host doctor [--config PATH] [--profile NAME] [--verbose] [--json]'");
}

static void ct_host_config_diagnostic(const char *operation,
                                      enum ct_host_config_status status)
{
  if (status == CT_HOST_CONFIG_ABSENT) {
    ct_cli_diagnostic(operation, "configuration",
                      "create or select a valid host profile, then retry");
  } else if (status == CT_HOST_CONFIG_INVALID) {
    ct_cli_diagnostic(operation, "configuration",
                      "correct the strict host profile and selected profile name, then retry");
  } else {
    ct_cli_diagnostic(operation, "configuration",
                      "make the selected host profile readable and stable, then retry");
  }
}

static bool ct_host_mount_plan_grammar_is_safe(const char *grammar)
{
  const unsigned char *cursor = (const unsigned char *)grammar;
  if (cursor == NULL || cursor[0] == '\0') return false;
  while (*cursor != '\0') {
    if (*cursor < 0x20U || *cursor > 0x7eU) return false;
    ++cursor;
  }
  return true;
}

static void ct_host_mount_plan_diagnostic(
    const char *operation, enum ct_mount_plan_read_status status,
    const struct ct_mount_plan_metadata *metadata)
{
  const char *action = "read the exact mount plan again after fixing its storage, then retry";
  switch (status) {
  case CT_MOUNT_PLAN_READ_FUTURE: {
    const char *actual = metadata == NULL ||
                                 !ct_host_mount_plan_grammar_is_safe(metadata->grammar)
                             ? "unknown" : metadata->grammar;
    (void)fprintf(stderr,
                  "container-tools: %s: mount-plan: unsupported grammar: "
                  "expected %s, actual %s; use a compatible container-tools "
                  "version or regenerate the mount plan, then retry\n",
                  operation, CT_MANIFEST_GRAMMAR, actual);
    return;
  }
  case CT_MOUNT_PLAN_READ_ABSENT:
    action = "publish or bind the selected profile mount plan, then retry";
    break;
  case CT_MOUNT_PLAN_READ_MALFORMED:
    action = "regenerate the selected profile mount plan using the supported grammar, then retry";
    break;
  case CT_MOUNT_PLAN_READ_DIGEST_MISMATCH:
    action = "republish the mount plan from its source launcher, then retry";
    break;
  case CT_MOUNT_PLAN_READ_SEMANTIC_INVALID:
    action = "regenerate a mount plan compatible with the selected profile, then retry";
    break;
  case CT_MOUNT_PLAN_READ_CHANGED:
    action = "wait for mount-plan publication to finish, then retry";
    break;
  case CT_MOUNT_PLAN_READ_IO:
    action = "make the selected profile mount plan readable and stable, then retry";
    break;
  case CT_MOUNT_PLAN_READ_OK:
    return;
  }
  ct_cli_diagnostic(operation, "mount-plan", action);
}

static void ct_host_executable_diagnostic(enum ct_executable_status status)
{
  const char *category = "executable";
  const char *action = "use a compatible executable in the selected root, then retry";
  switch (status) {
  case CT_EXECUTABLE_NOT_FOUND:
    category = "executable-not-found";
    action = "install the command in the selected root or add its directory to profile path, then retry";
    break;
  case CT_EXECUTABLE_INACCESSIBLE:
    category = "executable-inaccessible";
    action = "grant execute access to the selected-root command, then retry";
    break;
  case CT_EXECUTABLE_LOADER:
    category = "loader";
    action = "install a compatible dynamic loader in the selected root, then retry";
    break;
  case CT_EXECUTABLE_SHEBANG:
    category = "shebang";
    action = "use a supported absolute interpreter or GNU env shebang, then retry";
    break;
  case CT_EXECUTABLE_IO:
    action = "make the selected-root executable readable and stable, then retry";
    break;
  case CT_EXECUTABLE_INCOMPATIBLE:
  case CT_EXECUTABLE_OK:
    break;
  }
  ct_cli_diagnostic("host exec", category, action);
}

static void ct_host_backend_diagnostic(
    enum ct_nested_pre_dispatch_failure failure)
{
  const char *category = "backend";
  const char *action = "repair backend setup and retry";
  switch (failure) {
  case CT_NESTED_PRE_DISPATCH_TOOL_MISSING:
    category = "backend-tool-missing";
    action = "install Bubblewrap or PRoot, or select an eligible backend, then retry";
    break;
  case CT_NESTED_PRE_DISPATCH_POLICY_DENIED:
    category = "backend-policy";
    action = "select a backend permitted by the profile access and degradation policy, then retry";
    break;
  case CT_NESTED_PRE_DISPATCH_PROBE_TIMEOUT:
    category = "backend-probe-timeout";
    action = "fix backend startup latency or select another eligible backend, then retry";
    break;
  case CT_NESTED_PRE_DISPATCH_PROBE_FAILED:
    category = "backend-probe-failed";
    action = "repair the selected backend installation or policy, then retry";
    break;
  case CT_NESTED_PRE_DISPATCH_CLEANUP_UNCERTAIN:
    category = "backend-cleanup";
    action = "wait for backend cleanup, repair supervisor support, then retry";
    break;
  case CT_NESTED_PRE_DISPATCH_TRAMPOLINE:
    category = "trampoline";
    action = "restart with a working container-tools executable, then retry";
    break;
  case CT_NESTED_PRE_DISPATCH_COMMAND_BUILD:
    category = "backend-command";
    action = "free resources, verify selected-root mappings, then retry";
    break;
  case CT_NESTED_PRE_DISPATCH_DIAGNOSTIC_OUTPUT:
    category = "diagnostic-output";
    action = "restore a writable stderr destination, then retry";
    break;
  case CT_NESTED_PRE_DISPATCH_NONE:
    return;
  }
  ct_cli_diagnostic("host exec", category, action);
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

static int ct_host_verbose_selection(const char *operation, const char *backend)
{
  if (operation == NULL || backend == NULL) return 1;
  return fprintf(stderr, "container-tools: host %s: selection backend=%s\n",
                 operation, backend) < 0
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
        ct_host_usage("host exec");
        ct_path_map_destroy(&map);
        return CT_EXIT_USAGE;
      }
      backend = arguments[++index];
    }
    else if (strcmp(arguments[index], "--allow-degraded=rewrite") == 0 && allow_rewrite == 0) allow_rewrite = 1;
    else if (strcmp(arguments[index], "--verbose") == 0 && verbose == 0) verbose = 1;
    else if (strcmp(arguments[index], "--") == 0) { separator = index; break; }
    else {
      ct_host_usage("host exec");
      ct_path_map_destroy(&map);
      return CT_EXIT_USAGE;
    }
  }
  if (separator < 0 || separator + 1 >= argument_count ||
      (backend != NULL && ct_host_backend_is_valid(backend) == 0)) {
    ct_host_usage("host exec");
    ct_path_map_destroy(&map);
    return CT_EXIT_USAGE;
  }
  workspace = calloc(1U, sizeof(*workspace));
  if (workspace == NULL) {
    ct_cli_diagnostic("host exec", "resources",
                      "free memory or reduce concurrent work, then retry");
    return 125;
  }
  loaded = ct_host_config_load(config_path, profile_name, &workspace->profile);
  if (loaded == CT_HOST_CONFIG_ABSENT && config_path == NULL && profile_name == NULL) { ct_host_default_profile(&workspace->profile); loaded = CT_HOST_CONFIG_OK; }
  if (loaded != CT_HOST_CONFIG_OK) {
    ct_host_config_diagnostic("host exec", loaded);
    ct_path_map_destroy(&map);
    free(workspace);
    return 125;
  }
  if (stat(workspace->profile.root, &status) != 0 || !S_ISDIR(status.st_mode) ||
      ct_path_map_set_root(&map, workspace->profile.root) != 0) {
    ct_cli_diagnostic("host exec", "selected-root",
                      "configure an existing selected-root directory, then retry");
    ct_path_map_destroy(&map);
    free(workspace);
    return 125;
  }
  if (verbose != 0 &&
      ct_host_verbose_selection("exec", backend == NULL ? "automatic" : backend) != 0) {
    ct_cli_diagnostic("host exec", "diagnostic-output",
                      "restore a writable stderr destination, then retry");
    ct_path_map_destroy(&map);
    free(workspace);
    return 125;
  }
  ct_path_map_manifest_init(&manifest, &map, &workspace->profile);
  if (workspace->profile.mount_plan_configured != 0) {
    const enum ct_mount_plan_read_status mount_status = ct_mount_plan_read_status(
        workspace->profile.mount_plan, &workspace->metadata,
        ct_path_map_manifest_entry, &manifest);

    if (mount_status != CT_MOUNT_PLAN_READ_OK) {
      ct_host_mount_plan_diagnostic("host exec", mount_status,
                                    &workspace->metadata);
      ct_path_map_destroy(&map);
      free(workspace);
      return 125;
    }
    if (ct_path_map_manifest_eligible(&workspace->profile, &workspace->metadata,
                                      &manifest) != 0) {
      ct_cli_diagnostic("host exec", "mount-plan",
                        "regenerate a mount plan compatible with the selected profile, then retry");
      ct_path_map_destroy(&map);
      free(workspace);
      return 125;
    }
  }
  if (ct_host_add_projections(&workspace->profile, &map) != 0 ||
      ct_path_map_sort(&map) != 0) {
    ct_cli_diagnostic("host exec", "projection",
                      "make configured projections exist and non-conflicting, then retry");
    ct_path_map_destroy(&map);
    free(workspace);
    return 126;
  }
  if (getcwd(workspace->caller_cwd, sizeof(workspace->caller_cwd)) == NULL ||
      ct_path_map_cwd(&map, workspace->caller_cwd,
                      workspace->profile.cwd_unmapped,
                      workspace->target_cwd) != 0) {
    ct_cli_diagnostic("host exec", "cwd",
                      "run from a mapped directory or set cwd_unmapped to root, then retry");
    ct_path_map_destroy(&map);
    free(workspace);
    return 126;
  }
  if (ct_path_map_environment(&workspace->profile, workspace->environment,
                              &environment_count) != 0 ||
      environment_count == 0U ||
      ct_host_process_environment(workspace, environment_count) != 0) {
    ct_cli_diagnostic("host exec", "environment",
                      "correct the selected profile environment and path entries, then retry");
    ct_path_map_destroy(&map);
    free(workspace);
    return 126;
  }
  executable_result = ct_executable_resolve(
      &workspace->profile, &map, arguments[separator + 1],
      &workspace->executable);
  if (executable_result != CT_EXECUTABLE_OK) {
    ct_host_executable_diagnostic(executable_result);
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
    enum ct_nested_pre_dispatch_failure dispatch_failure;
    trampoline_descriptor = open("/proc/self/exe", O_RDONLY);
    control_descriptor = ct_executable_control_open(CT_BUILD_IDENTITY);
    if (trampoline_descriptor < 0 || control_descriptor < 0) {
      if (trampoline_descriptor >= 0) (void)close(trampoline_descriptor);
      if (control_descriptor >= 0) (void)close(control_descriptor);
      ct_executable_close(&workspace->executable);
      ct_cli_diagnostic("host exec", "trampoline",
                        "restart with a working container-tools executable, then retry");
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
    dispatch_result = ct_backend_nested_execute_detailed(
        &request, backend, allow_rewrite, &dispatch_failure);
    (void)close(trampoline_descriptor); (void)close(control_descriptor);
    ct_executable_close(&workspace->executable);
    ct_path_map_destroy(&map);
    free(workspace);
    ct_host_backend_diagnostic(dispatch_failure);
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
    else {
      ct_host_usage("host doctor");
      ct_path_map_destroy(&map);
      return CT_EXIT_USAGE;
    }
  }
  workspace = calloc(1U, sizeof(*workspace));
  if (workspace == NULL) {
    ct_cli_diagnostic("host doctor", "report",
                      "free resources and retry the complete diagnosis");
    goto invalid;
  }
  memset(&mount_plan, 0, sizeof(mount_plan));
  loaded = ct_host_config_load(config_path, profile_name, &workspace->profile);
  if (loaded == CT_HOST_CONFIG_ABSENT && config_path == NULL && profile_name == NULL) { ct_host_default_profile(&workspace->profile); loaded = CT_HOST_CONFIG_OK; }
  if (loaded != CT_HOST_CONFIG_OK) {
    ct_host_config_diagnostic("host doctor", loaded);
    goto invalid;
  }
  if (stat(workspace->profile.root, &status) != 0 || !S_ISDIR(status.st_mode) ||
      ct_path_map_set_root(&map, workspace->profile.root) != 0) {
    ct_cli_diagnostic("host doctor", "selected-root",
                      "configure an existing selected-root directory, then retry");
    goto invalid;
  }
  if (verbose != 0 &&
      ct_host_verbose_selection("doctor", "bubblewrap,proot,rewrite") != 0) {
    ct_cli_diagnostic("host doctor", "diagnostic-output",
                      "restore a writable stderr destination, then retry");
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
        ct_host_mount_plan_diagnostic("host doctor", manifest_status,
                                      &workspace->metadata);
        goto invalid;
      }
      mount_status = ct_host_mount_status(manifest_status);
      if (mount_status == NULL) {
        ct_host_mount_plan_diagnostic("host doctor", manifest_status,
                                      &workspace->metadata);
        goto invalid;
      }
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
       ct_path_map_sort(&map) != 0)) {
    ct_cli_diagnostic("host doctor", "projection",
                      "make configured projections exist and non-conflicting, then retry");
    goto invalid;
  }
  inherited_descriptor = ct_host_inherited_descriptor();
  trampoline_descriptor = open("/proc/self/exe", O_RDONLY);
  control_descriptor = ct_executable_control_open(CT_BUILD_IDENTITY);
  if (trampoline_descriptor < 0 || control_descriptor < 0) {
    ct_cli_diagnostic("host doctor", "trampoline",
                      "restart with a working container-tools executable, then retry");
    goto invalid;
  }
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
           : ct_backend_nested_blocked(&request, "mount-plan-unavailable", reports)) != 0) {
    ct_cli_diagnostic("host doctor", "backend-probe",
                      "repair backend cleanup or supervisor support, then retry");
    goto invalid;
  }
  if ((json != 0 ? ct_doctor_json(stdout, &workspace->profile, &mount_plan, reports)
                 : ct_host_doctor_human(&workspace->profile, &mount_plan, reports)) != 0) {
    ct_cli_diagnostic("host doctor", "report",
                      "retry after restoring the output destination");
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
  return 125;
}

struct ct_mount_plan_command_output {
  struct sigaction previous;
  int active;
};

static int ct_mount_plan_command_output_begin(
    struct ct_mount_plan_command_output *output)
{
  struct sigaction ignored;
  if (output == NULL || sigaction(SIGPIPE, NULL, &output->previous) != 0) return 1;
  memset(&ignored, 0, sizeof(ignored));
  ignored.sa_handler = SIG_IGN;
  if (sigemptyset(&ignored.sa_mask) != 0 ||
      sigaction(SIGPIPE, &ignored, NULL) != 0) return 1;
  output->active = 1;
  return 0;
}

static int ct_mount_plan_command_output_end(
    struct ct_mount_plan_command_output *output)
{
  int result = 0;
  if (output != NULL && output->active != 0) {
    if (sigaction(SIGPIPE, &output->previous, NULL) != 0) result = 1;
    output->active = 0;
  }
  return result;
}

static int ct_mount_plan_usage(const char *command)
{
  const char *arguments = "location [--help]";
  if (strcmp(command, "inspect") == 0) {
    arguments = "inspect [--json] [--] [PATH|DIGEST]";
  } else if (strcmp(command, "compare") == 0) {
    arguments = "compare [--json] [--] LEFT [RIGHT]";
  } else if (strcmp(command, "clear") == 0) {
    arguments = "clear [--help]";
  }
  return fprintf(stdout, "usage: container-tools mount plan %s\n", arguments) < 0 ||
         fflush(stdout) != 0 || ferror(stdout) != 0;
}

static int ct_mount_plan_diagnostic(const char *operation, const char *category,
                                    const char *action)
{
  ct_cli_diagnostic(operation, category, action);
  return fflush(stderr) != 0 || ferror(stderr) != 0;
}

static const char *ct_mount_plan_default_path(void)
{
#ifdef CT_MOUNT_PLAN_TEST_SEAM
  const char *path = getenv("CT_MOUNT_PLAN_TEST_DEFAULT_PATH");
  if (path != NULL && path[0] != '\0') return path;
#endif
  return "/.container-tools-mount-plan";
}

static int ct_mount_plan_is_digest(const char *value)
{
  size_t index;

  if (value == NULL || strnlen(value, 65U) != 64U) return 0;
  for (index = 0U; index < 64U; ++index) {
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) return 0;
  }
  return 1;
}

/* Resolve only a bare content digest; prefixed values remain explicit paths. */
static int ct_mount_plan_digest_path(const char *digest, char path[4096])
{
  char state_root[4096];
  int written;

  if (ct_mount_plan_state_root(state_root) != 0) return 1;
  written = snprintf(path, 4096U, "%s/%s.manifest", state_root, digest);
  return written < 0 || written >= 4096;
}

static int ct_mount_plan_read_diagnostic(const char *operation,
                                         enum ct_mount_plan_read_status status)
{
  const char *action = "publish or select a valid stable mount plan, then retry";
  if (status == CT_MOUNT_PLAN_READ_FUTURE) {
    action = "use a compatible container-tools version or regenerate the mount plan, then retry";
  } else if (status == CT_MOUNT_PLAN_READ_CHANGED) {
    action = "wait for mount-plan publication to finish, then retry";
  }
  (void)ct_mount_plan_diagnostic(operation, "mount-plan", action);
  return 125;
}

static int ct_mount_plan_clear_command(int argument_count, char **arguments)
{
  struct ct_mount_plan_command_output output = {0};
  char state_root[4096];
  int result = 125;

  if (ct_mount_plan_command_output_begin(&output) != 0) return 125;
  if (argument_count == 1 && strcmp(arguments[0], "--help") == 0) {
    if (ct_mount_plan_usage("clear") != 0) {
      (void)ct_mount_plan_diagnostic("mount plan clear", "report",
                                     "restore the output destination and retry");
    } else {
      result = 0;
    }
    goto complete;
  }
  if (argument_count != 0) {
    result = ct_mount_plan_diagnostic(
                 "mount plan clear", "usage",
                 "use 'container-tools mount plan clear [--help]'") != 0
                 ? 125
                 : CT_EXIT_USAGE;
    goto complete;
  }
  if (ct_mount_plan_state_root(state_root) != 0 ||
      ct_mount_plan_clear(state_root) != 0) {
    (void)ct_mount_plan_diagnostic(
        "mount plan clear", "cleanup",
        "repair the private mount-plan cache and retry");
    goto complete;
  }
  result = 0;
complete:
  return ct_mount_plan_command_output_end(&output) != 0 ? 125 : result;
}

static int ct_mount_plan_location_command(int argument_count, char **arguments)
{
  struct ct_mount_plan_command_output output = {0};
  char state_root[4096];
  int result = 125;

  if (ct_mount_plan_command_output_begin(&output) != 0) return 125;
  if (argument_count == 1 && strcmp(arguments[0], "--help") == 0) {
    if (ct_mount_plan_usage("location") != 0) {
      (void)ct_mount_plan_diagnostic("mount plan location", "report",
                                     "restore the output destination and retry");
    } else {
      result = 0;
    }
    goto complete;
  }
  if (argument_count != 0) {
    result = ct_mount_plan_diagnostic(
                 "mount plan location", "usage",
                 "use 'container-tools mount plan location [--help]'") != 0
                 ? 125
                 : CT_EXIT_USAGE;
    goto complete;
  }
  if (ct_mount_plan_state_root(state_root) != 0) {
    (void)ct_mount_plan_diagnostic(
        "mount plan location", "configuration",
        "set CT_MOUNT_PLAN_STATE_ROOT, XDG_STATE_HOME, or HOME to a safe absolute path and retry");
    goto complete;
  }
  if (fprintf(stdout, "%s\n", state_root) < 0 || fflush(stdout) != 0 ||
      ferror(stdout) != 0) {
    (void)ct_mount_plan_diagnostic("mount plan location", "report",
                                   "restore the output destination and retry");
    goto complete;
  }
  result = 0;
complete:
  return ct_mount_plan_command_output_end(&output) != 0 ? 125 : result;
}

static int ct_mount_plan_inspect(int argument_count, char **arguments)
{
  struct ct_mount_plan_report report;
  struct ct_mount_plan_command_output output = {0};
  const char *path = ct_mount_plan_default_path();
  const char *requested_digest = NULL;
  char digest_path[4096];
  int json = 0;
  int path_separator = 0;
  int result = 125;
  enum ct_mount_plan_read_status status;

  if (ct_mount_plan_command_output_begin(&output) != 0) return 125;
  if (argument_count == 1 && strcmp(arguments[0], "--help") == 0) {
    if (ct_mount_plan_usage("inspect") != 0) {
      ct_mount_plan_diagnostic("mount plan inspect", "report",
                               "restore the output destination and retry");
    } else {
      result = 0;
    }
    goto complete;
  }
  if (argument_count > 0 && strcmp(arguments[0], "--json") == 0) {
    json = 1;
    ++arguments;
    --argument_count;
  }
  if (argument_count > 0 && strcmp(arguments[0], "--") == 0) {
    ++arguments;
    --argument_count;
    path_separator = 1;
  }
  if (argument_count > 1 ||
      (argument_count == 1 && (arguments[0][0] == '\0' ||
       (path_separator == 0 && strncmp(arguments[0], "--", 2U) == 0)))) {
    ct_mount_plan_diagnostic("mount plan inspect", "usage",
                             "use 'container-tools mount plan inspect [--json] [--] [PATH|DIGEST]'");
    result = CT_EXIT_USAGE;
    goto complete;
  }
  if (argument_count == 1) {
    path = arguments[0];
    if (ct_mount_plan_is_digest(path) != 0) {
      requested_digest = path;
      if (ct_mount_plan_digest_path(path, digest_path) != 0) {
        ct_mount_plan_diagnostic(
            "mount plan inspect", "configuration",
            "set CT_MOUNT_PLAN_STATE_ROOT, XDG_STATE_HOME, or HOME to a safe absolute path and retry");
        goto complete;
      }
      path = digest_path;
    }
  }
  ct_mount_plan_report_init(&report);
  status = ct_mount_plan_report_read(path, &report);
  if (status != CT_MOUNT_PLAN_READ_OK) {
    result = ct_mount_plan_read_diagnostic("mount plan inspect", status);
    goto complete;
  }
  if (requested_digest != NULL &&
      strcmp(report.metadata.digest, requested_digest) != 0) {
    ct_mount_plan_report_destroy(&report);
    result = ct_mount_plan_read_diagnostic(
        "mount plan inspect", CT_MOUNT_PLAN_READ_DIGEST_MISMATCH);
    goto complete;
  }
  if ((json != 0 ? ct_mount_plan_report_write_json(stdout, &report)
                  : ct_mount_plan_report_write_human(stdout, &report)) != 0) {
    ct_mount_plan_report_destroy(&report);
    ct_mount_plan_diagnostic("mount plan inspect", "report",
                             "restore the output destination and retry");
    goto complete;
  }
  ct_mount_plan_report_destroy(&report);
  result = 0;
complete:
  return ct_mount_plan_command_output_end(&output) != 0 ? 125 : result;
}

static int ct_mount_plan_compare(int argument_count, char **arguments)
{
  struct ct_mount_plan_report left, right;
  struct ct_mount_plan_command_output output = {0};
  const char *right_path = ct_mount_plan_default_path();
  int json = 0;
  int path_separator = 0;
  int equal;
  int result = 125;
  enum ct_mount_plan_read_status status;

  if (ct_mount_plan_command_output_begin(&output) != 0) return 125;
  if (argument_count == 1 && strcmp(arguments[0], "--help") == 0) {
    if (ct_mount_plan_usage("compare") != 0) {
      ct_mount_plan_diagnostic("mount plan compare", "report",
                               "restore the output destination and retry");
    } else {
      result = 0;
    }
    goto complete;
  }
  if (argument_count > 0 && strcmp(arguments[0], "--json") == 0) {
    json = 1;
    ++arguments;
    --argument_count;
  }
  if (argument_count > 0 && strcmp(arguments[0], "--") == 0) {
    ++arguments;
    --argument_count;
    path_separator = 1;
  }
  if (argument_count < 1 || argument_count > 2 || arguments[0][0] == '\0' ||
      (path_separator == 0 && strncmp(arguments[0], "--", 2U) == 0) ||
      (argument_count == 2 && (arguments[1][0] == '\0' ||
       (path_separator == 0 && strncmp(arguments[1], "--", 2U) == 0)))) {
    ct_mount_plan_diagnostic("mount plan compare", "usage",
                             "use 'container-tools mount plan compare [--json] [--] LEFT [RIGHT]'");
    result = CT_EXIT_USAGE;
    goto complete;
  }
  if (argument_count == 2) right_path = arguments[1];
  ct_mount_plan_report_init(&left);
  ct_mount_plan_report_init(&right);
  status = ct_mount_plan_report_read(arguments[0], &left);
  if (status != CT_MOUNT_PLAN_READ_OK) {
    result = ct_mount_plan_read_diagnostic("mount plan compare", status);
    goto complete;
  }
  status = ct_mount_plan_report_read(right_path, &right);
  if (status != CT_MOUNT_PLAN_READ_OK) {
    ct_mount_plan_report_destroy(&left);
    result = ct_mount_plan_read_diagnostic("mount plan compare", status);
    goto complete;
  }
  equal = strcmp(left.metadata.digest, right.metadata.digest) == 0;
  if ((json != 0 ? ct_mount_plan_report_compare_write_json(stdout, equal, &left, &right)
                 : ct_mount_plan_report_compare_write_human(stdout, equal, &left, &right)) != 0) {
    ct_mount_plan_report_destroy(&left);
    ct_mount_plan_report_destroy(&right);
    ct_mount_plan_diagnostic("mount plan compare", "report",
                             "restore the output destination and retry");
    goto complete;
  }
  ct_mount_plan_report_destroy(&left);
  ct_mount_plan_report_destroy(&right);
  result = equal != 0 ? 0 : 1;
complete:
  return ct_mount_plan_command_output_end(&output) != 0 ? 125 : result;
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
    ct_cli_diagnostic("command line", "usage",
                      "use 'container-tools --help' for valid syntax");
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
  if (parsed.command == CT_COMMAND_MOUNT_PLAN_INSPECT) {
    return ct_mount_plan_inspect(argument_count - 4, arguments + 4);
  }
  if (parsed.command == CT_COMMAND_MOUNT_PLAN_COMPARE) {
    return ct_mount_plan_compare(argument_count - 4, arguments + 4);
  }
  if (parsed.command == CT_COMMAND_MOUNT_PLAN_CLEAR) {
    return ct_mount_plan_clear_command(argument_count - 4, arguments + 4);
  }
  if (parsed.command == CT_COMMAND_MOUNT_PLAN_LOCATION) {
    return ct_mount_plan_location_command(argument_count - 4, arguments + 4);
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
