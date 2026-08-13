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

static int has_arguments(const struct ct_nested_command *command,
                         const char *first, const char *second,
                         const char *third)
{
  size_t index;
  for (index = 0U; index + 2U < command->count; ++index) {
    if (strcmp(command->arguments[index], first) == 0 &&
        strcmp(command->arguments[index + 1U], second) == 0 &&
        strcmp(command->arguments[index + 2U], third) == 0) return 1;
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
  char workspace[4096], source[4096], absent[4096], usr[4096], bin[4096];
  char opt[4096];
  char *payload[] = {"/bin/true", "argument", NULL};
  int trampoline;
  int control;
  int inherited;
  int temporary;
  struct stat source_metadata;
  struct ct_host_profile profile;
  struct ct_path_map map;
  struct ct_path_map nested_map;
  struct ct_nested_request request;
  struct ct_nested_command command;
  if (mkdtemp(root) == NULL ||
      snprintf(workspace, sizeof(workspace), "%s/workspace", root) >= (int)sizeof(workspace) ||
      snprintf(source, sizeof(source), "%s-source", root) >= (int)sizeof(source) ||
      snprintf(absent, sizeof(absent), "%s/absent", root) >= (int)sizeof(absent) ||
      snprintf(usr, sizeof(usr), "%s/usr", root) >= (int)sizeof(usr) ||
      snprintf(bin, sizeof(bin), "%s/bin", root) >= (int)sizeof(bin) ||
      snprintf(opt, sizeof(opt), "%s/opt", root) >= (int)sizeof(opt) ||
      mkdir(workspace, 0700) != 0 || mkdir(source, 0700) != 0 ||
      mkdir(usr, 0700) != 0 || mkdir(opt, 0700) != 0 ||
      symlink("usr", bin) != 0) return 1;
  memset(&profile, 0, sizeof(profile)); strcpy(profile.root, root); strcpy(profile.root_access, "inherit");
  ct_path_map_init(&map);
  if (ct_path_map_set_root(&map, root) != 0 || ct_path_map_add(&map, source, "/workspace", "read-only", 1U, 0U) != 0 ||
      ct_path_map_add(&map, source, "/opt/msk/npm-global", "inherit", 1U, 1U) != 0 ||
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
      ct_backend_bubblewrap_arguments(&request, 0, &command) != 0) return 3;
  if (strcmp(command.arguments[0], "/fixture/bwrap") != 0 ||
      !has_arguments(&command, "--tmpfs", "/", "--symlink") ||
      !has_arguments(&command, "--symlink", "usr", "/bin")) return 3;
  if (!has_argument(&command, "/opt") || !has_argument(&command, "/opt/msk") ||
      !has_argument(&command, "/opt/msk/npm-global")) return 3;
  if (!has_arguments(&command, "--ro-bind", "/proc", "/proc") ||
      !has_arguments(&command, "--ro-bind", "/sys", "/sys") ||
      !has_arguments(&command, "--dev-bind", "/dev", "/dev") ||
      has_arguments(&command, "--bind", root, "/") ||
      has_arguments(&command, "--ro-bind", root, "/") ||
      !has_argument(&command, "--ro-bind") ||
      !has_argument(&command, "/proc/self/fd/9") ||
      !has_argument(&command, "--descriptor-fd") ||
      !has_argument(&command, "200") || has_argument(&command, "--preserve-fds") ||
      !has_argument(&command, "argument") ||
      has_argument(&command, "--unshare-all")) return 3;
  ct_nested_command_destroy(&command);
  strcpy(profile.root_access, "read-only");
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_bubblewrap_arguments(&request, 0, &command) != 0 ||
      !has_arguments(&command, "--remount-ro", "/", "--chdir")) return 4;
  ct_nested_command_destroy(&command);
  strcpy(profile.root_access, "inherit");
  ct_path_map_init(&nested_map);
  if (ct_path_map_set_root(&nested_map, root) != 0 ||
      ct_path_map_add(&nested_map, source, "/workspace", "inherit", 0U, 0U) !=
          0 ||
      ct_path_map_add(&nested_map, usr, "/workspace/sub", "inherit", 0U,
                      1U) != 0 ||
      ct_path_map_sort(&nested_map) != 0) return 4;
  request.map = &nested_map;
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_bubblewrap_arguments(&request, 1, &command) !=
          CT_NESTED_BUILD_UNSUPPORTED) return 4;
  ct_nested_command_destroy(&command);
  ct_path_map_destroy(&nested_map);
  request.map = &map;
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
                  unlink(bin) != 0 || rmdir(usr) != 0 || rmdir(opt) != 0 ||
                  rmdir(workspace) != 0 || rmdir(root) != 0 ? 5 : 0;
}
