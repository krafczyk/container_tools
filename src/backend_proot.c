/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_proot.h"

#include <string.h>
#include <sys/stat.h>

static int ct_proot_proc_target(const struct ct_nested_request *request)
{
  char target[CT_HOST_PATH_MAX];
  struct stat source;
  struct stat destination;
  return ct_path_map_root_path(request->profile->root, "/proc", target) != 0 ||
                 stat("/proc", &source) != 0 || stat(target, &destination) != 0 ||
                 !S_ISDIR(source.st_mode) || !S_ISDIR(destination.st_mode)
             ? 1
             : 0;
}

int ct_backend_proot_arguments(const struct ct_nested_request *request,
                               int probe, struct ct_nested_command *command)
{
  size_t index;
  const char *tool;
  if (request == NULL || request->profile == NULL || request->map == NULL ||
      request->cwd == NULL || command == NULL || command->arguments == NULL ||
      request->trampoline_descriptor < 0 || request->control_descriptor < 0 ||
      ct_backend_nested_requires_read_only(request) != 0 ||
      ct_proot_proc_target(request) != 0) {
    return 1;
  }
  tool = request->proot_path == NULL ? "proot" : request->proot_path;
  if (ct_nested_command_add(command, tool) != 0 ||
      ct_nested_command_add(command, "-r") != 0 ||
      ct_nested_command_add(command, request->profile->root) != 0) {
    return 1;
  }
  for (index = 0U; index < request->map->count; ++index) {
    const struct ct_path_map_entry *entry = &request->map->entries[index];
    if (strcmp(entry->target, "/proc") == 0 ||
        ct_nested_command_add(command, "-b") != 0 ||
        ct_nested_command_add_pair(command, entry->visible, ":",
                                   entry->target) != 0) {
      return 1;
    }
  }
  if (ct_nested_command_add(command, "-b") != 0 ||
      ct_nested_command_add_pair(command, "/proc", ":", "/proc") != 0 ||
      ct_nested_command_add(command, "-w") != 0 ||
      ct_nested_command_add(command, request->cwd) != 0 ||
      ct_nested_command_add_trampoline(request, probe, command) != 0) {
    return 1;
  }
  return 0;
}
