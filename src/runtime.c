/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "runtime.h"

#include "backend_outer.h"
#include "host_projection.h"
#include "mount_plan.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char *realpath(const char *restrict path, char *restrict resolved_path);

#define CT_RUNTIME_MAX_ARGUMENTS 1024U
#define CT_RUNTIME_MAX_ENTRIES 128U
#define CT_RUNTIME_MAX_MOUNTS (CT_RUNTIME_MAX_ENTRIES * 3U + 4U)
#define CT_RUNTIME_STRING_SLOTS (CT_RUNTIME_MAX_ENTRIES + CT_RUNTIME_MAX_MOUNTS)
#define CT_RUNTIME_PATH_MAX 4096U

struct ct_runtime_bind { const char *source; const char *target; };

static int ct_runtime_backend(const char *value)
{
  return value != NULL && (strcmp(value, "--docker") == 0 || strcmp(value, "--podman") == 0 ||
                           strcmp(value, "--singularity") == 0 || strcmp(value, "--apptainer") == 0);
}

static const char *ct_runtime_backend_name(const char *option)
{
  return option + 2;
}

static int ct_runtime_path_valid(const char *path)
{
  return path != NULL && path[0] == '/' && strchr(path, ':') == NULL && strchr(path, ',') == NULL && strchr(path, '\n') == NULL &&
         strstr(path, "//") == NULL && strstr(path, "/./") == NULL && strstr(path, "/../") == NULL &&
         strcmp(path, "/.") != 0 && strcmp(path, "/..") != 0;
}

static int ct_runtime_env_valid(const char *assignment)
{
  const char *equals = assignment == NULL ? NULL : strchr(assignment, '=');
  const char *cursor;
  if (equals == NULL || equals == assignment || !(assignment[0] == '_' || (assignment[0] >= 'A' && assignment[0] <= 'Z') || (assignment[0] >= 'a' && assignment[0] <= 'z'))) return 0;
  for (cursor = assignment + 1; cursor < equals; ++cursor) if (!(*cursor == '_' || (*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z') || (*cursor >= '0' && *cursor <= '9'))) return 0;
  for (cursor = equals + 1; *cursor != '\0'; ++cursor) if (!((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z') || (*cursor >= '0' && *cursor <= '9') || strchr("_./:@%+ -", *cursor) != NULL)) return 0;
  return 1;
}

static void ct_runtime_print_dry(char *const arguments[])
{
  size_t index;
  for (index = 0U; arguments[index] != NULL; ++index) {
    const char *cursor;
    for (cursor = arguments[index]; *cursor != '\0'; ++cursor) {
      if (strchr(" \\;\t\n", *cursor) != NULL) (void)fputc('\\', stdout);
      (void)fputc(*cursor, stdout);
    }
    (void)fputc(' ', stdout);
  }
  (void)fputc('\n', stdout);
}

static int ct_runtime_append(char *arguments[], size_t *count, char *value)
{
  if (*count + 1U >= CT_RUNTIME_MAX_ARGUMENTS || value == NULL) return 1;
  arguments[(*count)++] = value;
  arguments[*count] = NULL;
  return 0;
}

static int ct_runtime_mount_descriptor(const char *backend, const struct ct_runtime_bind *bind, int read_only, char output[CT_RUNTIME_PATH_MAX], const char **flag)
{
  const int docker = strcmp(backend, "docker") == 0 || strcmp(backend, "podman") == 0;
  if (!ct_runtime_path_valid(bind->source) || !ct_runtime_path_valid(bind->target)) return 1;
  *flag = docker ? "--mount" : "--bind";
  { const int written = snprintf(output, CT_RUNTIME_PATH_MAX, docker ? (read_only != 0 ? "type=bind,source=%s,target=%s,readonly" : "type=bind,source=%s,target=%s") : (read_only != 0 ? "%s:%s:ro" : "%s:%s"), bind->source, bind->target); return written < 0 || (size_t)written >= CT_RUNTIME_PATH_MAX; }
}

static int ct_runtime_path_inside(const char *path, const char *root)
{
  const size_t root_length = root == NULL ? 0U : strlen(root);
  return path != NULL && root != NULL && strncmp(path, root, root_length) == 0 && (path[root_length] == '\0' || path[root_length] == '/');
}

static int ct_runtime_cache_root(char output[CT_RUNTIME_PATH_MAX])
{
  const char *override = getenv("CT_HOST_PROJECTION_CACHE_ROOT");
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (override != NULL && override[0] != '\0') return ct_runtime_path_valid(override) ? snprintf(output, CT_RUNTIME_PATH_MAX, "%s", override) < (int)CT_RUNTIME_PATH_MAX ? 0 : 1 : 1;
  if (runtime != NULL && runtime[0] == '/') return snprintf(output, CT_RUNTIME_PATH_MAX, "%s/container-tools/host-projection-v1", runtime) >= (int)CT_RUNTIME_PATH_MAX;
  return snprintf(output, CT_RUNTIME_PATH_MAX, "/tmp/container-tools-%lu/host-projection-v1", (unsigned long)geteuid()) >= (int)CT_RUNTIME_PATH_MAX;
}

static int ct_runtime_target_taken(const struct ct_runtime_bind binds[], size_t bind_count, const char *target)
{
  size_t index;
  for (index = 0U; index < bind_count; ++index) if (strcmp(binds[index].target, target) == 0) return 1;
  return 0;
}

static int ct_runtime_targets_conflict(const char *left, const char *right)
{
  const size_t left_length = strlen(left), right_length = strlen(right);
  return strcmp(left, right) == 0 ||
         (left_length < right_length && strncmp(left, right, left_length) == 0 && right[left_length] == '/') ||
         (right_length < left_length && strncmp(right, left, right_length) == 0 && left[right_length] == '/');
}

static int ct_runtime_target_available(const struct ct_runtime_bind binds[], size_t bind_count,
                                       const char *target, const char *bootstrap)
{
  size_t index;
  if (ct_runtime_targets_conflict(target, "/.container-tools-mount-plan") ||
      (bootstrap != NULL && ct_runtime_targets_conflict(target, "/.container-tools-bootstrap"))) return 0;
  for (index = 0U; index < bind_count; ++index) if (strcmp(target, binds[index].target) == 0) return 0;
  return 1;
}

static int ct_runtime_explicit_target_available(const struct ct_runtime_bind binds[], size_t bind_count,
                                                const char *target)
{
  size_t index;
  for (index = 0U; index < bind_count; ++index) {
    if (ct_runtime_targets_conflict(binds[index].target, target)) return 0;
  }
  return !ct_runtime_targets_conflict(target, "/.container-tools-mount-plan");
}

static int ct_runtime_add_mask(const char *source, const char *destination, const char *private_root,
                                const struct ct_runtime_bind binds[], size_t bind_count,
                                const char *bootstrap, struct ct_runtime_bind masks[], size_t *mask_count)
{
  char target[CT_RUNTIME_PATH_MAX];
  const char *relative;
  if (ct_runtime_path_inside(source, private_root)) return 1;
  if (strcmp(source, "/") == 0) relative = private_root;
  else if (ct_runtime_path_inside(private_root, source)) relative = private_root + strlen(source);
  else return 0;
  if (snprintf(target, sizeof(target), "%s%s", destination, relative) >= (int)sizeof(target) || !ct_runtime_path_valid(target)) return 1;
  if (ct_runtime_target_taken(masks, *mask_count, target)) return 0;
  if (!ct_runtime_target_available(binds, bind_count, target, bootstrap)) return 1;
  if (*mask_count == CT_RUNTIME_MAX_MOUNTS || ct_runtime_path_valid(private_root) == 0) return 1;
  masks[*mask_count].source = private_root;
  /* The destination buffer belongs to the caller; it remains valid through dispatch. */
  masks[*mask_count].target = strdup(target);
  if (masks[*mask_count].target == NULL) return 1;
  ++*mask_count;
  return 0;
}

static int ct_runtime_state_root(char output[CT_RUNTIME_PATH_MAX])
{
  const char *override = getenv("CT_MOUNT_PLAN_STATE_ROOT");
  const char *base;
  if (override != NULL && override[0] != '\0') {
    if (!ct_runtime_path_valid(override)) return 1;
    { const int written = snprintf(output, CT_RUNTIME_PATH_MAX, "%s", override); return written < 0 || (size_t)written >= CT_RUNTIME_PATH_MAX; }
  }
  base = getenv("XDG_STATE_HOME");
  if (base == NULL || base[0] == '\0') base = getenv("HOME");
  if (base == NULL || base[0] != '/') return 1;
  { const int written = snprintf(output, CT_RUNTIME_PATH_MAX, "%s/%s", base, getenv("XDG_STATE_HOME") != NULL && getenv("XDG_STATE_HOME")[0] != '\0' ? "container-tools/mount-plans/v1" : ".local/state/container-tools/mount-plans/v1"); return written < 0 || (size_t)written >= CT_RUNTIME_PATH_MAX; }
}

int ct_runtime_foreground_command(int argument_count, char *const arguments[], int shell_mode)
{
  const char *backend, *host_root = "auto", *bootstrap = NULL, *container_shell = "/bin/sh";
  struct ct_runtime_bind binds[CT_RUNTIME_MAX_ENTRIES];
  struct ct_runtime_bind masks[CT_RUNTIME_MAX_MOUNTS];
  struct ct_host_projection projection;
  const char *environment[CT_RUNTIME_MAX_ENTRIES];
  struct ct_mount_plan_entry entries[CT_RUNTIME_MAX_ENTRIES * 2U + 1U];
  char resolved_bind_sources[CT_RUNTIME_MAX_ENTRIES][CT_RUNTIME_PATH_MAX];
  char mount_strings[CT_RUNTIME_STRING_SLOTS][CT_RUNTIME_PATH_MAX];
  char manifest_path[CT_RUNTIME_PATH_MAX], state_root[CT_RUNTIME_PATH_MAX], cache_root[CT_RUNTIME_PATH_MAX], uid_gid[64], cwd[CT_RUNTIME_PATH_MAX];
  char *command[CT_RUNTIME_MAX_ARGUMENTS] = {NULL};
  size_t bind_count = 0U, environment_count = 0U, entry_count = 0U, mask_count = 0U, mount_string_count = CT_RUNTIME_MAX_ENTRIES, payload_index = 1U, index, command_count = 0U;
  int delimited = 0, dry, remote, refresh = 0, result = 125;

  if (argument_count < 2 || arguments == NULL || !ct_runtime_backend(arguments[0])) { (void)fputs("container-tools: exec: container backend is required\n", stderr); return 64; }
  backend = ct_runtime_backend_name(arguments[0]);
  for (index = 1U; index < (size_t)argument_count; ) {
    const char *option = arguments[index];
    if (strcmp(option, "--") == 0) { delimited = 1; payload_index = index + 1U; break; }
    if (strcmp(option, "--ct-bind") == 0) {
      const char *separator;
      if (index + 1U >= (size_t)argument_count || bind_count == CT_RUNTIME_MAX_ENTRIES) return 64;
      separator = strchr(arguments[index + 1U], ':');
      if (separator == NULL || separator == arguments[index + 1U] || !ct_runtime_path_valid(separator + 1U) || strchr(separator + 1U, ':') != NULL) return 64;
      mount_strings[bind_count][0] = '\0';
      if ((size_t)(separator - arguments[index + 1U]) >= sizeof(mount_strings[bind_count])) return 64;
      memcpy(mount_strings[bind_count], arguments[index + 1U], (size_t)(separator - arguments[index + 1U]));
      mount_strings[bind_count][separator - arguments[index + 1U]] = '\0';
      if (!ct_runtime_path_valid(mount_strings[bind_count]) ||
          strcmp(separator + 1U, "/.container-tools-mount-plan") == 0 ||
          strncmp(separator + 1U, "/.container-tools-mount-plan/", 30U) == 0 ||
          access(mount_strings[bind_count], F_OK) != 0) return 1;
      binds[bind_count].source = mount_strings[bind_count]; binds[bind_count].target = separator + 1U; ++bind_count; index += 2U; continue;
    }
    if (strcmp(option, "--ct-env") == 0) { if (index + 1U >= (size_t)argument_count || environment_count == CT_RUNTIME_MAX_ENTRIES || !ct_runtime_env_valid(arguments[index + 1U])) return 64; environment[environment_count++] = arguments[index + 1U]; index += 2U; continue; }
    if (strcmp(option, "--ct-bootstrap") == 0) { if (bootstrap != NULL || index + 1U >= (size_t)argument_count || !ct_runtime_path_valid(arguments[index + 1U]) || access(arguments[index + 1U], R_OK | X_OK) != 0) return 64; bootstrap = arguments[index + 1U]; index += 2U; continue; }
    if (strcmp(option, "--ct-container-shell") == 0) { if (index + 1U >= (size_t)argument_count || arguments[index + 1U][0] != '/') return 64; container_shell = arguments[index + 1U]; index += 2U; continue; }
    if (strcmp(option, "--ct-host-root") == 0) { if (index + 1U >= (size_t)argument_count || (strcmp(arguments[index + 1U], "auto") != 0 && strcmp(arguments[index + 1U], "required") != 0 && strcmp(arguments[index + 1U], "disabled") != 0)) return 64; host_root = arguments[index + 1U]; index += 2U; continue; }
    if (strcmp(option, "--ct-host-root-refresh") == 0) { refresh = 1; ++index; continue; }
    break;
  }
  if (!delimited) payload_index = index;
  if (payload_index >= (size_t)argument_count || (bootstrap != NULL && !delimited) || (shell_mode != 0 && bootstrap == NULL && payload_index + 1U != (size_t)argument_count) || (shell_mode == 0 && payload_index + 1U >= (size_t)argument_count && bootstrap != NULL)) return 64;
  dry = getenv("CT_DRY_RUN") != NULL && getenv("CT_DRY_RUN")[0] != '\0';
  remote = !ct_host_projection_endpoint_is_local(backend);
  if (getcwd(cwd, sizeof(cwd)) == NULL) return 125;
  { const int projection_status = !dry && !remote ? ct_host_projection_prepare(backend, arguments[payload_index], host_root, refresh, &projection) : 0;
  if (projection_status == 2) { (void)fputs("container-tools: host projection cleanup is unconfirmed\n", stderr); return 1; }
  if (!dry && !remote && projection_status != 0) {
    if (strcmp(host_root, "required") == 0) { (void)fputs("container-tools: complete host projection is unavailable\n", stderr); return 1; }
    if (ct_host_projection_set_none(backend, &projection) != 0) return 1;
  } else if (dry || remote) {
    if (ct_host_projection_set_none(backend, &projection) != 0) return 1;
  } }
  if (strcmp(host_root, "required") == 0 && (remote || strcmp(projection.strategy, "none") == 0 || strcmp(projection.completeness, "complete") != 0)) { (void)fputs("container-tools: complete host projection is unavailable\n", stderr); return 1; }
  if (!dry && !remote) {
    struct ct_mount_plan plan;
    if (projection.entry_count + bind_count + (bootstrap != NULL ? 1U : 0U) > sizeof(entries) / sizeof(entries[0])) return 125;
    if (ct_runtime_state_root(state_root) != 0) return 1;
    if (ct_runtime_cache_root(cache_root) != 0) return 1;
    for (index = 0U; index < bind_count; ++index) {
      char resolved[CT_RUNTIME_PATH_MAX];
      if (ct_runtime_path_inside(binds[index].source, state_root) || ct_runtime_path_inside(binds[index].source, cache_root) ||
          realpath(binds[index].source, resolved) == NULL ||
          snprintf(resolved_bind_sources[index], sizeof(resolved_bind_sources[index]), "%s", resolved) >= (int)sizeof(resolved_bind_sources[index]) ||
          ct_runtime_path_inside(resolved, state_root) || ct_runtime_path_inside(resolved, cache_root) ||
           !ct_runtime_explicit_target_available(binds, index, binds[index].target)) { (void)fputs("container-tools: container bind source or target exposes private state\n", stderr); return 1; }
    }
    for (index = 0U; index < projection.entry_count; ) {
      size_t other;
      if (ct_runtime_path_inside(projection.entries[index].source, state_root) || ct_runtime_path_inside(projection.entries[index].source, cache_root) || ct_runtime_path_inside(projection.entries[index].target, state_root) || ct_runtime_path_inside(projection.entries[index].target, cache_root)) {
        projection.entries[index] = projection.entries[projection.entry_count - 1U];
        --projection.entry_count;
        (void)snprintf(projection.completeness, sizeof(projection.completeness), "partial");
        continue;
      }
      for (other = 0U; other < bind_count; ++other) if (ct_runtime_targets_conflict(projection.entries[index].destination, binds[other].target)) return 1;
      ++index;
    }
    if (strcmp(host_root, "required") == 0 && strcmp(projection.completeness, "complete") != 0) { (void)fputs("container-tools: complete host projection is unavailable\n", stderr); return 1; }
    entry_count = 0U;
    for (index = 0U; index < projection.entry_count; ++index) entries[entry_count++] = (struct ct_mount_plan_entry){"generated-host-root", projection.entries[index].destination, projection.entries[index].target, "inherit", (strcmp(backend, "docker") == 0 || strcmp(backend, "podman") == 0) ? "non-recursive" : "runtime-default"};
    for (index = 0U; index < bind_count; ++index) entries[entry_count++] = (struct ct_mount_plan_entry){"explicit", binds[index].target, binds[index].target, "inherit", "runtime-default"};
    if (bootstrap != NULL) entries[entry_count++] = (struct ct_mount_plan_entry){"bootstrap-internal", "/.container-tools-bootstrap", "/.container-tools-bootstrap", "read-only", "runtime-default"};
    plan.backend = backend; plan.strategy = projection.strategy; plan.completeness = projection.completeness; plan.group_mode = projection.group_mode; plan.entries = entries; plan.entry_count = entry_count;
    if (ct_mount_plan_publish(&plan, state_root, manifest_path) != 0) { (void)fputs("container-tools: cannot publish mount plan\n", stderr); return 1; }
    for (index = 0U; index < projection.entry_count; ++index) {
      if (ct_runtime_add_mask(projection.entries[index].target, projection.entries[index].destination, state_root, binds, bind_count, bootstrap, masks, &mask_count) != 0 ||
          ct_runtime_add_mask(projection.entries[index].target, projection.entries[index].destination, cache_root, binds, bind_count, bootstrap, masks, &mask_count) != 0) return 1;
    }
    for (index = 0U; index < bind_count; ++index) {
      if (ct_runtime_add_mask(resolved_bind_sources[index], binds[index].target, state_root, binds, bind_count, bootstrap, masks, &mask_count) != 0 ||
          ct_runtime_add_mask(resolved_bind_sources[index], binds[index].target, cache_root, binds, bind_count, bootstrap, masks, &mask_count) != 0) return 1;
    }
    if (strcmp(backend, "singularity") == 0 || strcmp(backend, "apptainer") == 0) {
      const char *home = getenv("HOME");
      char home_resolved[CT_RUNTIME_PATH_MAX];
      if (home != NULL && home[0] == '/' && realpath(home, home_resolved) != NULL &&
          (ct_runtime_add_mask(home_resolved, home_resolved, state_root, binds, bind_count, bootstrap, masks, &mask_count) != 0 ||
           ct_runtime_add_mask(home_resolved, home_resolved, cache_root, binds, bind_count, bootstrap, masks, &mask_count) != 0)) return 1;
      if (ct_runtime_add_mask(cwd, cwd, state_root, binds, bind_count, bootstrap, masks, &mask_count) != 0 ||
          ct_runtime_add_mask(cwd, cwd, cache_root, binds, bind_count, bootstrap, masks, &mask_count) != 0) return 1;
    }
  }
  if (snprintf(uid_gid, sizeof(uid_gid), "%lu:%lu", (unsigned long)geteuid(), (unsigned long)getegid()) >= (int)sizeof(uid_gid)) return 125;
  if (ct_runtime_append(command, &command_count, (char *)backend) != 0 || ct_runtime_append(command, &command_count, shell_mode != 0 && (strcmp(backend, "singularity") == 0 || strcmp(backend, "apptainer") == 0) && bootstrap == NULL ? "shell" : (strcmp(backend, "docker") == 0 || strcmp(backend, "podman") == 0 ? "run" : "exec")) != 0) return 125;
  if (strcmp(backend, "docker") == 0 || strcmp(backend, "podman") == 0) { if (ct_runtime_append(command, &command_count, "--rm") != 0 || (shell_mode != 0 && ct_runtime_append(command, &command_count, "-it") != 0) || (strcmp(backend, "podman") == 0 && ct_runtime_append(command, &command_count, "--userns=keep-id") != 0) || ct_runtime_append(command, &command_count, "--user") != 0 || ct_runtime_append(command, &command_count, uid_gid) != 0 || ct_runtime_append(command, &command_count, "-w") != 0 || ct_runtime_append(command, &command_count, cwd) != 0) return 125; }
  else if (ct_runtime_append(command, &command_count, "--pwd") != 0 || ct_runtime_append(command, &command_count, cwd) != 0) return 125;
  for (index = 0U; index < environment_count; ++index) if (ct_runtime_append(command, &command_count, "--env") != 0 || ct_runtime_append(command, &command_count, (char *)environment[index]) != 0) goto cleanup;
  for (index = 0U; index < projection.entry_count; ++index) { const char *flag; if (mount_string_count == CT_RUNTIME_STRING_SLOTS || ct_backend_outer_projection_mount(backend, projection.entries[index].source, projection.entries[index].destination, mount_strings[mount_string_count], sizeof(mount_strings[mount_string_count]), &flag) != 0 || ct_runtime_append(command, &command_count, (char *)flag) != 0 || ct_runtime_append(command, &command_count, mount_strings[mount_string_count++]) != 0) goto cleanup; }
  for (index = 0U; index < bind_count; ++index) { const char *flag; if (mount_string_count == CT_RUNTIME_STRING_SLOTS || ct_runtime_mount_descriptor(backend, &binds[index], 0, mount_strings[mount_string_count], &flag) != 0 || ct_runtime_append(command, &command_count, (char *)flag) != 0 || ct_runtime_append(command, &command_count, mount_strings[mount_string_count++]) != 0) goto cleanup; }
  if (bootstrap != NULL) { const char *flag; struct ct_runtime_bind bind = {bootstrap, "/.container-tools-bootstrap"}; if (mount_string_count == CT_RUNTIME_STRING_SLOTS || ct_runtime_mount_descriptor(backend, &bind, 1, mount_strings[mount_string_count], &flag) != 0 || ct_runtime_append(command, &command_count, (char *)flag) != 0 || ct_runtime_append(command, &command_count, mount_strings[mount_string_count++]) != 0) goto cleanup; }
  for (index = 0U; index < mask_count; ++index) { const char *flag; if (mount_string_count == CT_RUNTIME_STRING_SLOTS || ct_runtime_mount_descriptor(backend, &masks[index], 1, mount_strings[mount_string_count], &flag) != 0 || ct_runtime_append(command, &command_count, (char *)flag) != 0 || ct_runtime_append(command, &command_count, mount_strings[mount_string_count++]) != 0) goto cleanup; }
  if (!dry && !remote) { const char *flag; struct ct_runtime_bind bind = {manifest_path, "/.container-tools-mount-plan"}; if (mount_string_count == CT_RUNTIME_STRING_SLOTS || ct_runtime_mount_descriptor(backend, &bind, 1, mount_strings[mount_string_count], &flag) != 0 || ct_runtime_append(command, &command_count, (char *)flag) != 0 || ct_runtime_append(command, &command_count, mount_strings[mount_string_count++]) != 0) goto cleanup; }
  if (bootstrap != NULL) {
    if (ct_runtime_append(command, &command_count, arguments[payload_index]) != 0 || ct_runtime_append(command, &command_count, "/.container-tools-bootstrap") != 0) goto cleanup;
    if (shell_mode != 0) { if (ct_runtime_append(command, &command_count, (char *)container_shell) != 0 || ct_runtime_append(command, &command_count, "-i") != 0) goto cleanup; }
    else for (index = payload_index + 1U; index < (size_t)argument_count; ++index) if (ct_runtime_append(command, &command_count, arguments[index]) != 0) goto cleanup;
  } else {
    for (index = payload_index; index < (size_t)argument_count; ++index) if (ct_runtime_append(command, &command_count, arguments[index]) != 0) goto cleanup;
    if (shell_mode != 0 && (strcmp(backend, "docker") == 0 || strcmp(backend, "podman") == 0) &&
        (ct_runtime_append(command, &command_count, (char *)container_shell) != 0 ||
          ct_runtime_append(command, &command_count, "-i") != 0)) goto cleanup;
  }
  if (dry) { ct_runtime_print_dry(command); result = 0; }
  else result = ct_backend_outer_dispatch(command, command_count);
cleanup:
  for (index = 0U; index < mask_count; ++index) free((void *)masks[index].target);
  return result;
}
