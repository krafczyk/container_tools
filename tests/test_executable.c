/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "executable.h"
#include "shebang.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_executable(const char *path, const char *contents, mode_t mode)
{
  FILE *stream = fopen(path, "w");
  return stream == NULL || fputs(contents, stream) == EOF || fclose(stream) != 0 ||
                 chmod(path, mode) != 0
             ? 1
             : 0;
}

int main(void)
{
  char work[] = "/tmp/mkchad-v1/container-tools-c11/executable.XXXXXX";
  char control[4096], direct[4096], env_script[4096], env_split[4096];
  char env_assignment[4096], env_ignore[4096], env_malformed[4096];
  char recursive_a[4096], recursive_b[4096], missing[4096], nonexec[4096];
  char fifo[4096], proxy[4096], relative_proxy[4096], proxy_target[4096];
  char proxy_directory[4096], proxy_directory_link[4096], parent_proxy[4096];
  char proxy_loop[4096];
  const char identity[] = "unit-build-identity";
  int descriptor;
  int generated_control;
  int trampoline;
  int executable = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  struct ct_elf_info elf;
  static struct ct_host_profile profile;
  struct ct_path_map map;
  struct ct_executable resolved;
  if (mkdtemp(work) == NULL ||
      snprintf(control, sizeof(control), "%s/control", work) >= (int)sizeof(control) ||
      snprintf(direct, sizeof(direct), "%s/direct", work) >= (int)sizeof(direct) ||
      snprintf(env_script, sizeof(env_script), "%s/env", work) >= (int)sizeof(env_script) ||
      snprintf(env_split, sizeof(env_split), "%s/env-s", work) >= (int)sizeof(env_split) ||
      snprintf(env_assignment, sizeof(env_assignment), "%s/env-assignment", work) >= (int)sizeof(env_assignment) ||
       snprintf(env_ignore, sizeof(env_ignore), "%s/env-ignore", work) >= (int)sizeof(env_ignore) ||
       snprintf(env_malformed, sizeof(env_malformed), "%s/env-malformed", work) >= (int)sizeof(env_malformed) ||
      snprintf(recursive_a, sizeof(recursive_a), "%s/a", work) >= (int)sizeof(recursive_a) ||
      snprintf(recursive_b, sizeof(recursive_b), "%s/b", work) >= (int)sizeof(recursive_b) ||
      snprintf(missing, sizeof(missing), "%s/missing", work) >= (int)sizeof(missing) ||
      snprintf(nonexec, sizeof(nonexec), "%s/nonexec", work) >= (int)sizeof(nonexec) ||
      snprintf(proxy, sizeof(proxy), "%s/proxy", work) >= (int)sizeof(proxy) ||
      snprintf(relative_proxy, sizeof(relative_proxy), "%s/relative-proxy", work) >=
          (int)sizeof(relative_proxy) ||
      snprintf(proxy_target, sizeof(proxy_target), "%s/proxy-target", work) >=
          (int)sizeof(proxy_target) ||
      snprintf(proxy_directory, sizeof(proxy_directory), "%s/proxy-directory", work) >=
          (int)sizeof(proxy_directory) ||
      snprintf(proxy_directory_link, sizeof(proxy_directory_link), "%s/linked-directory", work) >=
          (int)sizeof(proxy_directory_link) ||
      snprintf(parent_proxy, sizeof(parent_proxy), "%s/proxy-directory/parent-proxy", work) >=
          (int)sizeof(parent_proxy) ||
      snprintf(proxy_loop, sizeof(proxy_loop), "%s/proxy-loop", work) >=
          (int)sizeof(proxy_loop)) return 1;
  if (snprintf(fifo, sizeof(fifo), "%s/fifo", work) >= (int)sizeof(fifo)) return 1;
  descriptor = open(control, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0 || executable < 0) return 1;
  if (dprintf(descriptor, "container-tools-control-v1\n%s\n", identity) < 0 || fsync(descriptor) != 0) return 2;
  if (ct_elf_inspect(executable, &elf) != 0 ||
      (elf.kind == CT_ELF_STATIC
           ? ct_executable_admit_trampoline(executable, descriptor, identity) != 0
           : ct_executable_admit_trampoline(executable, descriptor, identity) == 0)) return 3;
  if (ct_executable_admit_trampoline(executable, descriptor, "wrong") == 0) return 5;
  generated_control = ct_executable_control_open(identity);
  if (generated_control < 0 ||
      (elf.kind == CT_ELF_STATIC &&
       ct_executable_admit_trampoline(executable, generated_control, identity) != 0) ||
      close(generated_control) != 0) return 5;
  trampoline = ct_executable_trampoline_open();
  if (trampoline < 0) return 5;
  {
    char descriptor_path[64];
    char target[4096];
    ssize_t target_length;
    if (snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d",
                 trampoline) >= (int)sizeof(descriptor_path) ||
        (target_length = readlink(descriptor_path, target, sizeof(target) - 1U)) < 0 ||
        (size_t)target_length >= sizeof(target) - 1U) return 5;
    target[target_length] = '\0';
    if (strncmp(target, "/proc/", 6U) == 0 ||
        strncmp(target, "/dev/fd/", 8U) == 0 ||
        (fcntl(trampoline, F_GETFD) & FD_CLOEXEC) != 0 || close(trampoline) != 0) {
      return 5;
    }
  }
  if (write_executable(direct, "#!/bin/sh\n", 0700) != 0 ||
      write_executable(env_script, "#!/usr/bin/env sh\n", 0700) != 0 ||
      write_executable(env_split, "#!/usr/bin/env -S 'sh' -e\n", 0700) != 0 ||
       write_executable(env_assignment, "#!/usr/bin/env -S FOO=x sh\n", 0700) != 0 ||
       write_executable(env_ignore, "#!/usr/bin/env -S -i sh\n", 0700) != 0 ||
       write_executable(env_malformed, "#!/usr/bin/env -S sh 'unterminated\n", 0700) != 0 ||
      write_executable(missing, "#!/missing/interpreter\n", 0700) != 0 ||
      write_executable(nonexec, "#!/bin/sh\n", 0600) != 0 ||
      write_executable(proxy_target, "#!/bin/sh\n", 0700) != 0 ||
      symlink("/overlay/proxy-target", proxy) != 0 ||
      symlink("proxy-target", relative_proxy) != 0 ||
      mkdir(proxy_directory, 0700) != 0 ||
      symlink("/overlay", proxy_directory_link) != 0 ||
      symlink("../proxy-target", parent_proxy) != 0 ||
      symlink("proxy-loop", proxy_loop) != 0) return 6;
  if (mkfifo(fifo, 0600) != 0) return 6;
  {
    char line[8192];
    if (snprintf(line, sizeof(line), "#!%s\n", recursive_b) >= (int)sizeof(line) ||
        write_executable(recursive_a, line, 0700) != 0 ||
        snprintf(line, sizeof(line), "#!%s\n", recursive_a) >= (int)sizeof(line) ||
        write_executable(recursive_b, line, 0700) != 0) return 7;
  }
  memset(&profile, 0, sizeof(profile));
  strcpy(profile.root, "/");
  strcpy(profile.path[0], "/bin");
  profile.path_count = 1U;
  ct_path_map_init(&map);
  if (ct_path_map_set_root(&map, "/") != 0 ||
      ct_path_map_add(&map, work, "/overlay", "inherit", 0U, 0U) != 0 ||
       ct_executable_resolve(&profile, &map, "sh", &resolved) != CT_EXECUTABLE_OK ||
       resolved.stage_count != 1U || resolved.stages[0].elf.kind != CT_ELF_DYNAMIC ||
       resolved.stages[0].descriptor < 0 || resolved.loader_descriptor < 0) return 8;
  ct_executable_close(&resolved);
  if (ct_executable_resolve(&profile, &map, direct, &resolved) != CT_EXECUTABLE_OK ||
      resolved.stage_count != 2U || resolved.stages[0].is_shebang == 0 ||
      resolved.stages[1].is_shebang != 0) return 9;
  ct_executable_close(&resolved);
  if (ct_executable_resolve(&profile, &map, "/overlay/direct", &resolved) !=
          CT_EXECUTABLE_OK || strcmp(resolved.visible_path, direct) != 0) return 9;
  ct_executable_close(&resolved);
  if (ct_executable_resolve(&profile, &map, "/overlay/proxy", &resolved) !=
          CT_EXECUTABLE_OK || strcmp(resolved.target_path, "/overlay/proxy") != 0 ||
      strcmp(resolved.visible_path, proxy) != 0) return 9;
  ct_executable_close(&resolved);
  if (ct_executable_resolve(&profile, &map, "/overlay/relative-proxy", &resolved) !=
          CT_EXECUTABLE_OK || strcmp(resolved.visible_path, relative_proxy) != 0) return 9;
  ct_executable_close(&resolved);
  if (ct_executable_resolve(&profile, &map,
                            "/overlay/linked-directory/proxy-target", &resolved) !=
          CT_EXECUTABLE_OK || strstr(resolved.visible_path,
                                     "/linked-directory/proxy-target") == NULL) return 9;
  ct_executable_close(&resolved);
  if (ct_executable_resolve(&profile, &map,
                            "/overlay/proxy-directory/parent-proxy", &resolved) !=
          CT_EXECUTABLE_OK || strcmp(resolved.visible_path, parent_proxy) != 0) return 9;
  ct_executable_close(&resolved);
  if (ct_executable_resolve(&profile, &map, "/overlay/proxy-loop", &resolved) !=
      CT_EXECUTABLE_INCOMPATIBLE) return 9;
  if (ct_executable_resolve(&profile, &map, env_script, &resolved) != CT_EXECUTABLE_OK ||
      resolved.stage_count != 3U || resolved.stages[0].shebang.uses_env == 0) return 10;
  ct_executable_close(&resolved);
  if (ct_executable_resolve(&profile, &map, env_split, &resolved) != CT_EXECUTABLE_OK ||
       resolved.stage_count != 3U || resolved.stages[0].env_argument_count != 1U ||
       strcmp(resolved.stages[0].env_arguments[0], "-e") != 0) return 11;
  ct_executable_close(&resolved);
  if (ct_executable_resolve(&profile, &map, env_assignment, &resolved) !=
          CT_EXECUTABLE_OK || resolved.stage_count != 3U) return 11;
  ct_executable_close(&resolved);
  if (ct_executable_resolve(&profile, &map, env_ignore, &resolved) !=
          CT_EXECUTABLE_OK || resolved.stage_count != 3U) return 11;
  ct_executable_close(&resolved);
  if (
      ct_executable_resolve(&profile, &map, recursive_a, &resolved) !=
          CT_EXECUTABLE_SHEBANG ||
      ct_executable_resolve(&profile, &map, missing, &resolved) !=
          CT_EXECUTABLE_SHEBANG ||
      ct_executable_resolve(&profile, &map, nonexec, &resolved) !=
          CT_EXECUTABLE_INACCESSIBLE ||
       ct_executable_resolve(&profile, &map, fifo, &resolved) !=
           CT_EXECUTABLE_INCOMPATIBLE ||
       ct_executable_resolve(&profile, &map, env_malformed, &resolved) !=
           CT_EXECUTABLE_SHEBANG ||
      ct_executable_resolve(&profile, &map, "not-present-container-tools-test", &resolved) !=
          CT_EXECUTABLE_NOT_FOUND) return 12;
  ct_path_map_destroy(&map);
  if (close(executable) != 0 || close(descriptor) != 0 || unlink(control) != 0 ||
       unlink(direct) != 0 || unlink(env_script) != 0 || unlink(env_split) != 0 ||
      unlink(env_assignment) != 0 ||
       unlink(env_ignore) != 0 ||
       unlink(env_malformed) != 0 ||
      unlink(recursive_a) != 0 || unlink(recursive_b) != 0 || unlink(missing) != 0 ||
      unlink(nonexec) != 0 || unlink(fifo) != 0 || unlink(proxy) != 0 ||
      unlink(relative_proxy) != 0 || unlink(proxy_target) != 0 ||
      unlink(proxy_directory_link) != 0 || unlink(parent_proxy) != 0 ||
      unlink(proxy_loop) != 0 ||
      rmdir(proxy_directory) != 0 || rmdir(work) != 0) return 13;
  return 0;
}
