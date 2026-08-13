/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_proot.h"

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
  for (index = 0U; index < command->count; ++index) if (strcmp(command->arguments[index], value) == 0) return 1;
  return 0;
}

static int has_probe(const struct ct_nested_command *command,
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
        strcmp(command->arguments[index + 2U], "/") == 0 &&
        strcmp(command->arguments[index + 3U], device) == 0 &&
        strcmp(command->arguments[index + 4U], inode) == 0 &&
        strcmp(command->arguments[index + 5U], type) == 0) return 1;
  }
  return 0;
}

int main(void)
{
  char root[] = "/tmp/mkchad-v1/container-tools-c11/proot.XXXXXX";
  char *payload[] = {"/bin/true", NULL};
  int trampoline;
  int control;
  int temporary;
  struct stat root_metadata;
  struct ct_host_profile profile;
  struct ct_path_map map;
  struct ct_nested_request request;
  struct ct_nested_command command;
  if (mkdtemp(root) == NULL) return 1;
  memset(&profile, 0, sizeof(profile)); strcpy(profile.root, root); strcpy(profile.root_access, "inherit");
  ct_path_map_init(&map); if (ct_path_map_set_root(&map, root) != 0) return 2;
  temporary = open("/dev/null", O_RDONLY);
  trampoline = temporary < 0 ? -1 : dup2(temporary, 3);
  control = temporary < 0 ? -1 : dup2(temporary, 4);
  if (temporary < 0 || trampoline != 3 || control != 4 ||
      (temporary != 3 && temporary != 4 && close(temporary) != 0)) return 2;
  memset(&request, 0, sizeof(request)); request.profile = &profile; request.map = &map; request.cwd = "/";
  request.trampoline_descriptor = trampoline; request.control_descriptor = control; request.payload = payload; request.proot_path = "/fixture/proot";
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_proot_arguments(&request, 1, &command) != 0 ||
       strcmp(command.arguments[0], "/fixture/proot") != 0 || !has_argument(&command, "/proc:/proc") ||
       !has_argument(&command, "/proc/self/fd/3") || has_argument(&command, "-R") || has_argument(&command, "-S") || has_argument(&command, "-0")) return 3;
  ct_nested_command_destroy(&command);
  if (stat(root, &root_metadata) != 0 ||
      ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_proot_arguments(&request, 1, &command) != 0 ||
      !has_probe(&command, &root_metadata)) return 4;
  ct_nested_command_destroy(&command);
  strcpy(profile.root_access, "read-only");
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 || ct_backend_proot_arguments(&request, 1, &command) == 0) return 4;
  ct_nested_command_destroy(&command); ct_path_map_destroy(&map);
  return close(control) != 0 || close(trampoline) != 0 || rmdir(root) != 0 ? 5 : 0;
}
