/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_rewrite.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define CT_TEST_VFS_CAP_REVISION_2 0x02000000U
#define CT_TEST_VFS_CAP_EFFECTIVE 0x000001U

struct ct_test_vfs_cap_data {
  uint32_t magic_etc;
  struct {
    uint32_t permitted;
    uint32_t inheritable;
  } data[2];
};

static void initialize_executable(struct ct_executable *executable,
                                  int descriptor)
{
  size_t index;
  memset(executable, 0, sizeof(*executable));
  executable->descriptor = descriptor;
  executable->loader_descriptor = -1;
  for (index = 0U; index < CT_EXECUTABLE_MAX_STAGES; ++index) {
    executable->stages[index].descriptor = -1;
  }
}

static int warning_matches(void)
{
  struct ct_nested_backend_report outcomes[CT_NESTED_BACKEND_COUNT] = {{0}};
  FILE *stream = tmpfile();
  char output[512];
  size_t count;
  outcomes[CT_NESTED_BACKEND_BUBBLEWRAP].reason_code = "probe-failed";
  outcomes[CT_NESTED_BACKEND_PROOT].reason_code = "not-installed";
  if (stream == NULL ||
      ct_backend_rewrite_warning(stream, "full-root", outcomes) != 0 ||
      fseek(stream, 0L, SEEK_SET) != 0) {
    return 1;
  }
  count = fread(output, 1U, sizeof(output) - 1U, stream);
  output[count] = '\0';
  return fclose(stream) != 0 ||
                 strcmp(output,
                        "container-tools: host exec: requested semantics full-root; "
                        "full-root attempts: bubblewrap=probe-failed, proot=not-installed; "
                        "selected rewrite entry-point behavior; descendants do not receive "
                        "selected-root filesystem semantics and read-only access is not "
                        "enforced\n") != 0;
}

int main(void)
{
  struct ct_host_profile profile;
  struct ct_path_map map;
  struct ct_executable executable;
  struct ct_nested_request request;
  struct ct_nested_command command;
  char *payload[] = {"/entry", "argument", NULL};
  int descriptor = open("/bin/true", O_RDONLY | O_CLOEXEC);
  char stage_path[] = "/tmp/mkchad-v1/container-tools-c11/rewrite-stage.XXXXXX";
  char script_path[] = "/tmp/mkchad-v1/container-tools-c11/rewrite-script.XXXXXX";
  int stage_descriptor = mkstemp(stage_path);
  int script_descriptor = mkstemp(script_path);
  if (descriptor < 0 || stage_descriptor < 0 || script_descriptor < 0 ||
      fchmod(stage_descriptor, 0700) != 0 ||
      fchmod(script_descriptor, 0700) != 0) {
    return 1;
  }
  memset(&profile, 0, sizeof(profile)); strcpy(profile.root, "/"); strcpy(profile.semantics, "rewrite");
  ct_path_map_init(&map); if (ct_path_map_set_root(&map, "/") != 0) return 2;
  initialize_executable(&executable, descriptor);
  strcpy(executable.visible_path, "/bin/true"); strcpy(executable.target_path, "/bin/true"); executable.stage_count = 1U;
  strcpy(executable.stages[0].target_path, "/bin/true"); strcpy(executable.stages[0].visible_path, "/bin/true");
  executable.stages[0].descriptor = dup(descriptor); executable.stages[0].elf.kind = CT_ELF_STATIC;
  if (executable.stages[0].descriptor < 0) return 2;
  memset(&request, 0, sizeof(request)); request.profile = &profile; request.map = &map;
  request.executable = &executable; request.payload = payload;
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_rewrite_arguments(&request, &command) != 0 ||
      strcmp(command.arguments[0], "/bin/true") != 0 || strcmp(command.arguments[1], "argument") != 0 || command.arguments[2] != NULL) return 3;
  ct_nested_command_destroy(&command);
  strcpy(executable.stages[0].visible_path, "/bin/false");
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_rewrite_arguments(&request, &command) == 0) return 3;
  ct_nested_command_destroy(&command);
  strcpy(executable.stages[0].visible_path, "/bin/true");
  if (close(executable.stages[0].descriptor) != 0) return 3;
  executable.stage_count = 2U; executable.stages[0].is_shebang = 1;
  strcpy(executable.stages[0].target_path, "/script"); strcpy(executable.stages[0].shebang.argument, "-e");
  strcpy(executable.stages[0].visible_path, script_path);
  executable.stages[0].descriptor = script_descriptor;
  strcpy(executable.stages[1].target_path, "/bin/true"); strcpy(executable.stages[1].visible_path, "/bin/true");
  executable.stages[1].descriptor = stage_descriptor; executable.stages[1].elf.kind = CT_ELF_STATIC;
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_rewrite_arguments(&request, &command) != 0 ||
      strcmp(command.arguments[0], "/bin/true") != 0 || strcmp(command.arguments[1], "-e") != 0 ||
      strcmp(command.arguments[2], script_path) != 0 || strcmp(command.arguments[3], "argument") != 0) return 4;
  ct_nested_command_destroy(&command);
  executable.stages[0].shebang.uses_env = 1; strcpy(executable.stages[0].shebang.argument, "-S python -O");
  executable.stages[0].env_argument_count = 1U; strcpy(executable.stages[0].env_arguments[0], "-O");
  if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_rewrite_arguments(&request, &command) != 0 ||
      strcmp(command.arguments[0], "/bin/true") != 0 ||
      strcmp(command.arguments[1], "-O") != 0 ||
      strcmp(command.arguments[2], script_path) != 0 ||
      strcmp(command.arguments[3], "argument") != 0) return 5;
  ct_nested_command_destroy(&command);
  if (fchmod(stage_descriptor, 0700 | S_ISUID) != 0 ||
      ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_rewrite_arguments(&request, &command) == 0) return 6;
  ct_nested_command_destroy(&command);
  if (fchmod(stage_descriptor, 0700) != 0) return 7;
  executable.stages[1].descriptor = dup(descriptor);
  executable.stages[1].elf.kind = CT_ELF_DYNAMIC;
  strcpy(executable.stages[1].elf.interpreter, "/lib64/ld-linux-x86-64.so.2");
  executable.loader_descriptor = stage_descriptor;
  strcpy(executable.loader_visible_path, "/lib64/ld-linux-x86-64.so.2");
  if (executable.stages[1].descriptor < 0 ||
      fchmod(stage_descriptor, 0700 | S_ISUID) != 0 ||
      ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
      ct_backend_rewrite_arguments(&request, &command) == 0) return 8;
  ct_nested_command_destroy(&command);
  if (fchmod(stage_descriptor, 0700) != 0 ||
      close(executable.stages[1].descriptor) != 0) return 9;
  executable.stages[1].descriptor = stage_descriptor;
  executable.stages[1].elf.kind = CT_ELF_STATIC;
  executable.loader_descriptor = -1;
  {
    struct ct_test_vfs_cap_data capability;
    memset(&capability, 0, sizeof(capability));
    capability.magic_etc = CT_TEST_VFS_CAP_REVISION_2 | CT_TEST_VFS_CAP_EFFECTIVE;
    capability.data[0].permitted = 1U;
    if (fsetxattr(stage_descriptor, "security.capability", &capability,
                  sizeof(capability), 0) == 0) {
      if (ct_nested_command_init(&command, CT_NESTED_ARGUMENT_LIMIT) != 0 ||
          ct_backend_rewrite_arguments(&request, &command) == 0 ||
          fremovexattr(stage_descriptor, "security.capability") != 0) return 10;
      ct_nested_command_destroy(&command);
    } else if (errno != EOPNOTSUPP && errno != ENOTSUP && errno != EPERM &&
               errno != EACCES) {
      return 10;
    }
  }
  if (warning_matches() != 0) return 11;
  ct_path_map_destroy(&map); ct_executable_close(&executable);
  return unlink(stage_path) != 0 || unlink(script_path) != 0 ? 12 : 0;
}
