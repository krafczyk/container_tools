/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_bubblewrap.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int write_file(const char *path, const char *contents)
{
  const int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  const size_t length = strlen(contents);
  return descriptor < 0 || write(descriptor, contents, length) != (ssize_t)length ||
                 close(descriptor) != 0;
}

static int has_sequence(const struct ct_nested_command *command, const char *const values[],
                        size_t count)
{
  size_t index;
  if (command == NULL || values == NULL || count == 0U) return 0;
  for (index = 0U; index + count <= command->count; ++index) {
    size_t value;
    for (value = 0U; value < count; ++value) {
      if (strcmp(command->arguments[index + value], values[value]) != 0) break;
    }
    if (value == count) return 1;
  }
  return 0;
}

int main(void)
{
  char root[] = "/tmp/mkchad-v1/container-tools-c11/use-cases.XXXXXX";
  char proc[4096], workspace[4096], run[4096], slurm[4096], target_run[4096];
  char target_slurm[4096], source_workspace[4096], source_run[4096];
  char source_slurm[4096], cargo_toml[4096], slurm_conf[4096], socket_path[4096];
  char *cargo_payload[] = {"/usr/bin/cargo", "build", "--jobs=8", NULL};
  char *slurm_payload[] = {"/usr/bin/srun", "--job-name=fixture", "/bin/true", NULL};
  const char *const cargo_overlay[] = {"--bind", source_workspace, "/workspace"};
  const char *const slurm_config_overlay[] = {"--ro-bind", source_slurm, "/etc/slurm"};
  const char *const slurm_socket_overlay[] = {"--bind", source_run, "/run/slurm"};
  const char *const descriptor[] = {"--descriptor-fd", "200"};
  int trampoline;
  int control;
  int inherited;
  int temporary;
  int socket_descriptor;
  struct sockaddr_un address;
  struct stat socket_status;
  struct ct_host_profile profile;
  struct ct_path_map map;
  struct ct_nested_request request;
  struct ct_nested_command command;
  if (mkdtemp(root) == NULL ||
      snprintf(proc, sizeof(proc), "%s/proc", root) >= (int)sizeof(proc) ||
      snprintf(workspace, sizeof(workspace), "%s/workspace", root) >= (int)sizeof(workspace) ||
      snprintf(run, sizeof(run), "%s/run", root) >= (int)sizeof(run) ||
      snprintf(slurm, sizeof(slurm), "%s/etc", root) >= (int)sizeof(slurm) ||
      snprintf(target_run, sizeof(target_run), "%s/slurm", run) >= (int)sizeof(target_run) ||
      snprintf(target_slurm, sizeof(target_slurm), "%s/slurm", slurm) >= (int)sizeof(target_slurm) ||
      snprintf(source_workspace, sizeof(source_workspace), "%s-workspace", root) >= (int)sizeof(source_workspace) ||
      snprintf(source_run, sizeof(source_run), "%s-run", root) >= (int)sizeof(source_run) ||
      snprintf(source_slurm, sizeof(source_slurm), "%s-slurm", root) >= (int)sizeof(source_slurm) ||
      snprintf(cargo_toml, sizeof(cargo_toml), "%s/Cargo.toml", source_workspace) >= (int)sizeof(cargo_toml) ||
      snprintf(slurm_conf, sizeof(slurm_conf), "%s/slurm.conf", source_slurm) >= (int)sizeof(slurm_conf) ||
      snprintf(socket_path, sizeof(socket_path), "%s/slurmctld.sock", source_run) >= (int)sizeof(socket_path) ||
      mkdir(proc, 0700) != 0 || mkdir(workspace, 0700) != 0 || mkdir(run, 0700) != 0 ||
      mkdir(slurm, 0700) != 0 || mkdir(target_run, 0700) != 0 ||
      mkdir(target_slurm, 0700) != 0 || mkdir(source_workspace, 0700) != 0 ||
      mkdir(source_run, 0700) != 0 || mkdir(source_slurm, 0700) != 0 ||
      write_file(cargo_toml, "[package]\nname = \"fixture\"\n") != 0 ||
      write_file(slurm_conf, "ClusterName=fixture\n") != 0) return 1;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(socket_path) >= sizeof(address.sun_path)) return 1;
  strcpy(address.sun_path, socket_path);
  socket_descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_descriptor < 0 || bind(socket_descriptor, (const struct sockaddr *)&address,
                                    sizeof(address)) != 0 ||
      lstat(socket_path, &socket_status) != 0 || !S_ISSOCK(socket_status.st_mode)) return 1;
  memset(&profile, 0, sizeof(profile));
  strcpy(profile.root, root); strcpy(profile.root_access, "inherit");
  strcpy(profile.semantics, "full-root");
  ct_path_map_init(&map);
  if (ct_path_map_set_root(&map, root) != 0 ||
      ct_path_map_add(&map, source_workspace, "/workspace", "inherit", 0U, 0U) != 0 ||
      ct_path_map_add(&map, source_run, "/run/slurm", "inherit", 1U, 0U) != 0 ||
      ct_path_map_add(&map, source_slurm, "/etc/slurm", "read-only", 1U, 1U) != 0 ||
      ct_path_map_sort(&map) != 0) return 2;
  temporary = open("/dev/null", O_RDONLY);
  trampoline = temporary < 0 ? -1 : dup2(temporary, 7);
  control = temporary < 0 ? -1 : dup2(temporary, 8);
  inherited = temporary < 0 ? -1 : dup2(temporary, 200);
  if (temporary < 0 || trampoline != 7 || control != 8 || inherited != 200 ||
      (temporary != 7 && temporary != 8 && temporary != 200 && close(temporary) != 0)) return 2;
  memset(&request, 0, sizeof(request));
  request.profile = &profile; request.map = &map; request.cwd = "/workspace";
  request.payload = cargo_payload; request.trampoline_descriptor = trampoline;
  request.control_descriptor = control; request.inherited_descriptor = inherited;
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_bubblewrap_arguments(&request, 0, &command) != 0 ||
      !has_sequence(&command, cargo_overlay, sizeof(cargo_overlay) / sizeof(cargo_overlay[0])) ||
      !has_sequence(&command, slurm_config_overlay,
                    sizeof(slurm_config_overlay) / sizeof(slurm_config_overlay[0])) ||
      !has_sequence(&command, slurm_socket_overlay,
                    sizeof(slurm_socket_overlay) / sizeof(slurm_socket_overlay[0])) ||
      !has_sequence(&command, descriptor, sizeof(descriptor) / sizeof(descriptor[0])) ||
      !has_sequence(&command, (const char *const[]){"--chdir", "/workspace"}, 2U) ||
      !has_sequence(&command, (const char *const[]){"/usr/bin/cargo", "build", "--jobs=8"}, 3U)) return 3;
  ct_nested_command_destroy(&command);
  request.payload = slurm_payload;
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_bubblewrap_arguments(&request, 0, &command) != 0 ||
      !has_sequence(&command, (const char *const[]){"/usr/bin/srun", "--job-name=fixture", "/bin/true"}, 3U)) return 3;
  ct_nested_command_destroy(&command);
  strcpy(profile.root_access, "read-only");
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_bubblewrap_arguments(&request, 0, &command) != 0 ||
      !has_sequence(&command, (const char *const[]){"--ro-bind", root, "/"}, 3U)) return 3;
  ct_nested_command_destroy(&command); ct_path_map_destroy(&map);
  return close(socket_descriptor) != 0 || close(inherited) != 0 || close(control) != 0 ||
                 close(trampoline) != 0 || unlink(socket_path) != 0 ||
                 unlink(slurm_conf) != 0 || unlink(cargo_toml) != 0 ||
                 rmdir(source_slurm) != 0 || rmdir(source_run) != 0 ||
                 rmdir(source_workspace) != 0 || rmdir(target_slurm) != 0 ||
                 rmdir(target_run) != 0 || rmdir(slurm) != 0 || rmdir(run) != 0 ||
                 rmdir(workspace) != 0 || rmdir(proc) != 0 || rmdir(root) != 0
             ? 4
             : 0;
}
