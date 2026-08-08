/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_bubblewrap.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int has_argument(const struct ct_nested_command *command, const char *value)
{
  size_t index;
  for (index = 0U; index < command->count; ++index) {
    if (strcmp(command->arguments[index], value) == 0) return 1;
  }
  return 0;
}

static int has_probe(const struct ct_nested_command *command, const char *target,
                     const struct stat *metadata)
{
  char device[32];
  char inode[32];
  char type[32];
  size_t index;
  if (snprintf(device, sizeof(device), "%ju", (uintmax_t)metadata->st_dev) < 0 ||
      snprintf(inode, sizeof(inode), "%ju", (uintmax_t)metadata->st_ino) < 0 ||
      snprintf(type, sizeof(type), "%ju",
               (uintmax_t)(metadata->st_mode & S_IFMT)) < 0) {
    return 0;
  }
  for (index = 0U; index + 5U < command->count; ++index) {
    if (strcmp(command->arguments[index], "--probe") == 0 &&
        strcmp(command->arguments[index + 2U], target) == 0 &&
        strcmp(command->arguments[index + 3U], device) == 0 &&
        strcmp(command->arguments[index + 4U], inode) == 0 &&
        strcmp(command->arguments[index + 5U], type) == 0) return 1;
  }
  return 0;
}

int main(void)
{
  char root[] = "/tmp/mkchad-v1/container-tools-c11/bwrap.XXXXXX";
  char proc[4096], workspace[4096], source[4096], absent[4096];
  char *payload[] = {"/bin/true", "argument", NULL};
  int trampoline;
  int control;
  int inherited;
  int temporary;
  struct stat source_metadata;
  struct ct_host_profile profile;
  struct ct_path_map map;
  struct ct_nested_request request;
  struct ct_nested_command command;
  if (mkdtemp(root) == NULL || snprintf(proc, sizeof(proc), "%s/proc", root) >= (int)sizeof(proc) ||
      snprintf(workspace, sizeof(workspace), "%s/workspace", root) >= (int)sizeof(workspace) ||
      snprintf(source, sizeof(source), "%s-source", root) >= (int)sizeof(source) ||
      snprintf(absent, sizeof(absent), "%s/absent", root) >= (int)sizeof(absent) ||
      mkdir(proc, 0700) != 0 || mkdir(workspace, 0700) != 0 || mkdir(source, 0700) != 0) return 1;
  memset(&profile, 0, sizeof(profile)); strcpy(profile.root, root); strcpy(profile.root_access, "inherit");
  ct_path_map_init(&map);
  if (ct_path_map_set_root(&map, root) != 0 || ct_path_map_add(&map, source, "/workspace", "read-only", 1U, 0U) != 0 ||
      ct_path_map_sort(&map) != 0) return 2;
  temporary = open("/dev/null", O_RDONLY);
  trampoline = temporary < 0 ? -1 : dup2(temporary, 9);
  control = temporary < 0 ? -1 : dup2(temporary, 10);
  inherited = temporary < 0 ? -1 : dup2(temporary, 200);
  if (temporary < 0 || trampoline != 9 || control != 10 || inherited != 200 ||
      (temporary != 9 && temporary != 10 && temporary != 200 &&
       close(temporary) != 0)) return 2;
  memset(&request, 0, sizeof(request));
  request.profile = &profile; request.map = &map; request.cwd = "/workspace";
  request.trampoline_descriptor = trampoline; request.control_descriptor = control;
  request.inherited_descriptor = inherited;
  request.payload = payload; request.bubblewrap_path = "/fixture/bwrap";
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_bubblewrap_arguments(&request, 0, &command) != 0 ||
      strcmp(command.arguments[0], "/fixture/bwrap") != 0 ||
       !has_argument(&command, "--ro-bind") || !has_argument(&command, "/proc/self/fd/9") ||
       !has_argument(&command, "--descriptor-fd") || !has_argument(&command, "200") ||
       has_argument(&command, "--preserve-fds") ||
       !has_argument(&command, "argument") || has_argument(&command, "--unshare-all")) return 3;
  ct_nested_command_destroy(&command);
  if (stat(source, &source_metadata) != 0 ||
      ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_bubblewrap_arguments(&request, 1, &command) != 0 ||
      !has_probe(&command, "/workspace", &source_metadata)) return 4;
  ct_nested_command_destroy(&command);
  if (rmdir(source) != 0) return 4;
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_bubblewrap_arguments(&request, 1, &command) == 0 || access(absent, F_OK) == 0) return 4;
  ct_nested_command_destroy(&command);
  ct_path_map_destroy(&map);
  return close(inherited) != 0 || close(control) != 0 || close(trampoline) != 0 ||
                 rmdir(workspace) != 0 || rmdir(proc) != 0 || rmdir(root) != 0 ? 5 : 0;
}
