/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_rewrite.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>

static int ct_rewrite_secure_descriptor(int descriptor)
{
  struct stat status;
  ssize_t capability_size;
  if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
      (status.st_mode & (S_ISUID | S_ISGID)) != 0) {
    return 1;
  }
  errno = 0;
  capability_size = fgetxattr(descriptor, "security.capability", NULL, 0U);
  if (capability_size > 0) return 1;
  return capability_size == 0 || errno == ENODATA ? 0 : 1;
}

static int ct_rewrite_dynamic(const struct ct_nested_request *request,
                              const struct ct_executable_stage *stage,
                              struct ct_nested_command *command)
{
  static const char *const targets[] = {"/lib", "/lib64", "/usr/lib",
                                         "/usr/lib64"};
  char loader[CT_HOST_PATH_MAX];
  char libraries[CT_HOST_PATH_MAX];
  size_t index;
  size_t used = 0U;
  if (strstr(stage->elf.interpreter, "ld-musl") != NULL ||
      ct_host_copy_bounded(loader, sizeof(loader),
                           request->executable->loader_visible_path) != 0 ||
      ct_rewrite_secure_descriptor(request->executable->loader_descriptor) != 0) {
    return 1;
  }
  libraries[0] = '\0';
  for (index = 0U; index < sizeof(targets) / sizeof(targets[0]); ++index) {
    char visible[CT_HOST_PATH_MAX];
    struct stat status;
    int written;
    if (ct_path_map_visible(request->map, targets[index], visible) != 0 ||
        stat(visible, &status) != 0 || !S_ISDIR(status.st_mode)) {
      continue;
    }
    written = snprintf(libraries + used, sizeof(libraries) - used, "%s%s",
                       used == 0U ? "" : ":", visible);
    if (written < 0 || (size_t)written >= sizeof(libraries) - used) return 1;
    used += (size_t)written;
  }
  return used == 0U || ct_nested_command_add(command, loader) != 0 ||
                 ct_nested_command_add(command, "--inhibit-cache") != 0 ||
                 ct_nested_command_add(command, "--library-path") != 0 ||
                 ct_nested_command_add_pair(command, libraries, "", "") != 0
             ? 1
             : 0;
}

int ct_backend_rewrite_arguments(const struct ct_nested_request *request,
                                 struct ct_nested_command *command)
{
  const struct ct_executable_stage *final_stage;
  char final_visible[CT_HOST_PATH_MAX];
  size_t index;
  if (request == NULL || request->profile == NULL || request->map == NULL ||
      request->executable == NULL || request->payload == NULL ||
      request->payload[0] == NULL || command == NULL || command->arguments == NULL ||
      request->executable->stage_count == 0U) {
    return 1;
  }
  for (index = 0U; index < request->executable->stage_count; ++index) {
    if (ct_rewrite_secure_descriptor(
            request->executable->stages[index].descriptor) != 0) {
      return 1;
    }
  }
  final_stage = &request->executable->stages[request->executable->stage_count - 1U];
  if (final_stage->is_shebang != 0 || final_stage->elf.kind == CT_ELF_INVALID) {
    return 1;
  }
  if (ct_host_copy_bounded(final_visible, sizeof(final_visible),
                           final_stage->visible_path) != 0) {
    return 1;
  }
  if (final_stage->elf.kind == CT_ELF_DYNAMIC) {
    if (ct_rewrite_dynamic(request, final_stage, command) != 0 ||
        ct_nested_command_add_pair(command, final_visible, "", "") != 0) {
      return 1;
    }
  } else if (ct_nested_command_add_pair(command, final_visible, "", "") != 0) {
    return 1;
  }
  for (index = request->executable->stage_count - 1U; index-- > 0U;) {
    const struct ct_executable_stage *stage = &request->executable->stages[index];
    char visible[CT_HOST_PATH_MAX];
    if (stage->is_shebang == 0) continue;
    if (stage->env_argument_count > CT_EXECUTABLE_ENV_ARGUMENT_MAX) return 1;
    if (stage->shebang.uses_env != 0) {
      size_t argument_index;
      for (argument_index = 0U;
           argument_index < stage->env_argument_count; ++argument_index) {
        if (ct_nested_command_add(command,
                                  stage->env_arguments[argument_index]) != 0) {
          return 1;
        }
      }
    }
    if (stage->shebang.uses_env == 0 && stage->shebang.argument[0] != '\0' &&
        ct_nested_command_add(command, stage->shebang.argument) != 0) {
      return 1;
    }
    if (ct_host_copy_bounded(visible, sizeof(visible), stage->visible_path) != 0 ||
        ct_nested_command_add_pair(command, visible, "", "") != 0) {
      return 1;
    }
  }
  for (index = 1U; request->payload[index] != NULL; ++index) {
    if (ct_nested_command_add(command, request->payload[index]) != 0) return 1;
  }
  return ct_nested_command_finish(command);
}

int ct_backend_rewrite_warning(
    FILE *stream, const char *requested_semantics,
    const struct ct_nested_backend_report outcomes[CT_NESTED_BACKEND_COUNT])
{
  const char *bubblewrap = outcomes == NULL || outcomes[CT_NESTED_BACKEND_BUBBLEWRAP].reason_code == NULL
                                ? "not-attempted"
                                : outcomes[CT_NESTED_BACKEND_BUBBLEWRAP].reason_code;
  const char *proot = outcomes == NULL || outcomes[CT_NESTED_BACKEND_PROOT].reason_code == NULL
                          ? "not-attempted"
                          : outcomes[CT_NESTED_BACKEND_PROOT].reason_code;
  return stream == NULL || requested_semantics == NULL ||
                 fprintf(stream,
                         "container-tools: host exec: requested semantics %s; "
                         "full-root attempts: bubblewrap=%s, proot=%s; selected "
                         "rewrite entry-point behavior; descendants do not receive "
                         "selected-root filesystem semantics and read-only access is "
                         "not enforced\n",
                         requested_semantics, bubblewrap, proot) < 0
             ? 1
             : 0;
}
