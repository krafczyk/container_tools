/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_bubblewrap.h"

#include <string.h>
#include <sys/stat.h>

static int ct_bubblewrap_target(const struct ct_nested_request *request,
                                const char *target, const char *source)
{
  char lower[CT_HOST_PATH_MAX];
  struct stat destination;
  struct stat visible;
  if (ct_path_map_root_path(request->profile->root, target, lower) != 0 ||
      stat(lower, &destination) != 0 || stat(source, &visible) != 0) {
    return 1;
  }
  if (S_ISDIR(destination.st_mode) != S_ISDIR(visible.st_mode)) return 1;
  return !S_ISDIR(destination.st_mode) &&
                 (destination.st_mode & 0170000U) != (visible.st_mode & 0170000U)
             ? 1
             : 0;
}

int ct_backend_bubblewrap_arguments(const struct ct_nested_request *request,
                                    int probe,
                                    struct ct_nested_command *command)
{
  size_t index;
  const char *tool;
  if (request == NULL || request->profile == NULL || request->map == NULL ||
      request->cwd == NULL || command == NULL || command->arguments == NULL ||
      request->trampoline_descriptor < 0 || request->control_descriptor < 0 ||
      ct_bubblewrap_target(request, "/", request->profile->root) != 0 ||
      ct_bubblewrap_target(request, "/proc", "/proc") != 0) {
    return 1;
  }
  tool = request->bubblewrap_path == NULL ? "bwrap" : request->bubblewrap_path;
  if (ct_nested_command_add(command, tool) != 0 ||
      ct_nested_command_add(command,
                            strcmp(request->profile->root_access, "read-only") == 0
                                ? "--ro-bind"
                                : "--bind") != 0 ||
      ct_nested_command_add(command, request->profile->root) != 0 ||
      ct_nested_command_add(command, "/") != 0) {
    return 1;
  }
  for (index = 0U; index < request->map->count; ++index) {
    const struct ct_path_map_entry *entry = &request->map->entries[index];
    if (strcmp(entry->target, "/proc") == 0 ||
        ct_bubblewrap_target(request, entry->target, entry->visible) != 0 ||
        ct_nested_command_add(
            command, strcmp(entry->access, "read-only") == 0 ? "--ro-bind"
                                                               : "--bind") != 0 ||
        ct_nested_command_add(command, entry->visible) != 0 ||
        ct_nested_command_add(command, entry->target) != 0) {
      return 1;
    }
  }
  if (ct_nested_command_add(command, "--ro-bind") != 0 ||
      ct_nested_command_add(command, "/proc") != 0 ||
       ct_nested_command_add(command, "/proc") != 0 ||
       ct_nested_command_add(command, "--chdir") != 0 ||
       ct_nested_command_add(command, request->cwd) != 0 ||
       ct_nested_command_add_trampoline(request, probe, command) != 0) {
    return 1;
  }
  return 0;
}
