/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "runtime.h"

#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int ct_test_runtime_foreground_command(int argument_count,
                                              char *const arguments[])
{
  const pid_t child = fork();
  int status;
  if (child < 0) return 125;
  if (child == 0) {
    char expected[32];
    if (snprintf(expected, sizeof(expected), "%lu", (unsigned long)getpid()) >=
            (int)sizeof(expected) ||
        setenv("CT_NATIVE_TEST_EXPECT_PID", expected, 1) != 0) _exit(125);
    _exit(ct_runtime_foreground_command(argument_count, arguments, 0));
  }
  if (waitpid(child, &status, 0) != child) return 125;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
}

int main(void)
{
  char *dry[] = {"--docker", "--ct-host-root", "disabled", "image", "command"};
  char *duplicate_shell[] = {"--docker", "--ct-container-shell", "/bin/sh", "--ct-container-shell", "/bin/bash", "image", "command"};
  char *duplicate_host_root[] = {"--docker", "--ct-host-root", "required", "--ct-host-root", "disabled", "image", "command"};
  char *duplicate_refresh[] = {"--docker", "--ct-host-root-refresh", "--ct-host-root-refresh", "image", "command"};
  char work[] = "/tmp/mkchad-v1/container-tools-c11/native-runtime-test.XXXXXX";
  char fake[4096], source[4096], alias[4096], alias_binding[8192], binding[8192], readonly_binding[8192], invalid_binding[8192], reserved_binding[8192], log[4096], state[4096], mountinfo[4096], path[8192], apptainer[8192];
  char *launch[] = {"--docker", "--ct-host-root", "required", "--ct-bind", binding, "image", "command"};
  char *alias_launch[] = {"--docker", "--ct-host-root", "disabled", "--ct-bind", alias_binding, "image", "command"};
  char *readonly_launch[] = {"--docker", "--ct-host-root", "disabled", "--ct-bind", readonly_binding, "image", "command"};
  char *readonly_native_launch[] = {"--apptainer", "--ct-host-root", "disabled", "--ct-bind", readonly_binding, "image", "command"};
  char *invalid_launch[] = {"--docker", "--ct-host-root", "disabled", "--ct-bind", invalid_binding, "image", "command"};
  char *reserved_launch[] = {"--docker", "--ct-host-root", "disabled", "--ct-bind", reserved_binding, "image", "command"};
  FILE *stream;
  char contents[16384];
  size_t bytes;
  if (setenv("CT_DRY_RUN", "1", 1) != 0 || ct_runtime_foreground_command(5, dry, 0) != 0 ||
      ct_runtime_foreground_command(7, duplicate_shell, 0) != 64 ||
      ct_runtime_foreground_command(7, duplicate_host_root, 0) != 64 ||
      ct_runtime_foreground_command(5, duplicate_refresh, 0) != 64) return 1;
  if (unsetenv("CT_DRY_RUN") != 0 || mkdtemp(work) == NULL ||
      snprintf(fake, sizeof(fake), "%s/fake", work) >= (int)sizeof(fake) || mkdir(fake, 0700) != 0 ||
       snprintf(source, sizeof(source), "%s/source", work) >= (int)sizeof(source) || mkdir(source, 0700) != 0 ||
       snprintf(binding, sizeof(binding), "%s:/workspace", source) >= (int)sizeof(binding) ||
       snprintf(readonly_binding, sizeof(readonly_binding), "%s:/workspace:ro", source) >= (int)sizeof(readonly_binding) ||
       snprintf(invalid_binding, sizeof(invalid_binding), "%s:/workspace:rw", source) >= (int)sizeof(invalid_binding) ||
       snprintf(reserved_binding, sizeof(reserved_binding), "%s:/.container-tools-mount-plan:ro", source) >= (int)sizeof(reserved_binding) ||
      snprintf(log, sizeof(log), "%s/log", work) >= (int)sizeof(log) ||
       snprintf(state, sizeof(state), "%s/state", work) >= (int)sizeof(state) ||
       snprintf(mountinfo, sizeof(mountinfo), "%s/mountinfo", work) >= (int)sizeof(mountinfo) ||
       snprintf(path, sizeof(path), "%s/docker", fake) >= (int)sizeof(path) ||
       snprintf(apptainer, sizeof(apptainer), "%s/apptainer", fake) >= (int)sizeof(apptainer)) return 1;
  stream = fopen(mountinfo, "w");
  if (stream == NULL || fputs("1 0 0:1 / / rw - ext4 root rw\n", stream) == EOF || fclose(stream) != 0) return 1;
  stream = fopen(path, "w");
  if (stream == NULL || fputs("#!/bin/sh\ncase \"$1\" in run|exec) [ \"$$\" = \"$CT_NATIVE_TEST_EXPECT_PID\" ] || exit 97;; esac\nprintf '%s\\n' \"$@\" > \"$CT_NATIVE_TEST_LOG\"\nif [ \"$1\" = container ] && [ \"$2\" = inspect ]; then last=; for arg; do last=$arg; done; printf '%s\\n' \"$last\"; fi\n", stream) == EOF || fclose(stream) != 0 || chmod(path, 0700) != 0 || symlink(path, apptainer) != 0 ||
       setenv("PATH", fake, 1) != 0 || setenv("XDG_STATE_HOME", state, 1) != 0 || setenv("CT_NATIVE_TEST_LOG", log, 1) != 0 ||
       setenv("CT_HOST_PROJECTION_SOURCE_ROOT", source, 1) != 0 || setenv("CT_HOST_PROJECTION_MOUNTINFO", mountinfo, 1) != 0 ||
       setenv("CT_HOST_PROJECTION_CACHE_ROOT", state, 1) != 0 || setenv("CT_HOST_PROJECTION_FAKE_PROBE", "direct", 1) != 0 ||
       ct_test_runtime_foreground_command(7, launch) != 0) return 1;
  stream = fopen(log, "r");
  if (stream == NULL) return 1;
  bytes = fread(contents, 1U, sizeof(contents) - 1U, stream);
  if (fclose(stream) != 0 || bytes == 0U) return 1;
  contents[bytes] = '\0';
  if (strstr(contents, "/.container-tools-mount-plan,readonly") == NULL) return 1;
  if (ct_test_runtime_foreground_command(7, readonly_launch) != 0) return 1;
  stream = fopen(log, "r");
  if (stream == NULL) return 1;
  bytes = fread(contents, 1U, sizeof(contents) - 1U, stream);
  if (fclose(stream) != 0 || bytes == 0U) return 1;
  contents[bytes] = '\0';
  if (strstr(contents, "type=bind,source=") == NULL ||
      strstr(contents, ",target=/workspace,readonly") == NULL) return 1;
  if (ct_test_runtime_foreground_command(7, readonly_native_launch) != 0) return 1;
  stream = fopen(log, "r");
  if (stream == NULL) return 1;
  bytes = fread(contents, 1U, sizeof(contents) - 1U, stream);
  if (fclose(stream) != 0 || bytes == 0U) return 1;
  contents[bytes] = '\0';
  if (strstr(contents, ":/workspace:ro") == NULL ||
      ct_runtime_foreground_command(7, invalid_launch, 0) != 64 ||
      ct_runtime_foreground_command(7, reserved_launch, 0) != 1) return 1;
  if (snprintf(alias, sizeof(alias), "%s/alias", work) >= (int)sizeof(alias) ||
      symlink(work, alias) != 0 ||
      snprintf(alias_binding, sizeof(alias_binding), "%s:/workspace", alias) >= (int)sizeof(alias_binding) ||
      ct_test_runtime_foreground_command(7, alias_launch) != 0) return 1;
  stream = fopen(log, "r");
  if (stream == NULL) return 1;
  bytes = fread(contents, 1U, sizeof(contents) - 1U, stream);
  if (fclose(stream) != 0 || bytes == 0U) return 1;
  contents[bytes] = '\0';
  if (strstr(contents, "target=/workspace/state,readonly") == NULL) return 1;
  return 0;
}
