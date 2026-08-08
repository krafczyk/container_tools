/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "trampoline.h"
#include "executable.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int ct_trampoline_descriptor(const char *value, int *descriptor)
{
  char *end;
  long parsed;
  if (value == NULL || descriptor == NULL) return 1;
  errno = 0;
  parsed = strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < 0 ||
      parsed > 1048576L) {
    return 1;
  }
  *descriptor = (int)parsed;
  return 0;
}

static int ct_trampoline_cwd(const char *expected)
{
  char cwd[4096];
  return expected == NULL || expected[0] != '/' || getcwd(cwd, sizeof(cwd)) == NULL ||
         strcmp(cwd, expected) != 0;
}

static int ct_trampoline_uintmax(const char *value, uintmax_t *parsed)
{
  char *end;
  if (value == NULL || parsed == NULL || value[0] == '\0' || value[0] == '+' ||
      value[0] == '-') {
    return 1;
  }
  errno = 0;
  *parsed = strtoumax(value, &end, 10);
  return errno != 0 || end == value || *end != '\0';
}

static int ct_trampoline_projection(char *const arguments[5])
{
  struct stat metadata;
  uintmax_t device;
  uintmax_t inode;
  uintmax_t type;
  if (arguments == NULL || ct_trampoline_cwd(arguments[0]) != 0 ||
      arguments[1] == NULL || arguments[1][0] != '/' ||
      ct_trampoline_uintmax(arguments[2], &device) != 0 ||
      ct_trampoline_uintmax(arguments[3], &inode) != 0 ||
      ct_trampoline_uintmax(arguments[4], &type) != 0 ||
      stat(arguments[1], &metadata) != 0 ||
      (uintmax_t)metadata.st_dev != device ||
      (uintmax_t)metadata.st_ino != inode ||
      (uintmax_t)(metadata.st_mode & S_IFMT) != type) {
    return 1;
  }
  return 0;
}

static int ct_trampoline_inherited_descriptor(int descriptor)
{
  const int flags = descriptor < 3 ? 0 : fcntl(descriptor, F_GETFD);
  return descriptor >= 3 && (flags < 0 || (flags & FD_CLOEXEC) != 0);
}

int ct_trampoline_main(int argument_count, char **arguments, const char *build_identity)
{
  int control_descriptor;
  int inherited_descriptor = -1;
  int executable;
  int index = 3;
#ifdef CT_TRAMPOLINE_TEST_SEAM
  (void)build_identity;
#endif
  if (argument_count < 4 || strcmp(arguments[0], "--internal-trampoline") != 0 ||
      strcmp(arguments[1], "--control-fd") != 0 ||
      ct_trampoline_descriptor(arguments[2], &control_descriptor) != 0) return 125;
  if (strcmp(arguments[index], "--descriptor-fd") == 0) {
    if (++index >= argument_count ||
        ct_trampoline_descriptor(arguments[index++], &inherited_descriptor) != 0 ||
        inherited_descriptor < 3) return 125;
  }
  if (index >= argument_count) return 125;
  executable = open("/proc/self/exe", O_RDONLY);
  if (executable < 0 || ct_trampoline_inherited_descriptor(inherited_descriptor) != 0 ||
#ifdef CT_TRAMPOLINE_TEST_SEAM
      fcntl(control_descriptor, F_GETFD) < 0) {
#else
      ct_executable_admit_trampoline(executable, control_descriptor, build_identity) != 0) {
#endif
    if (executable >= 0) (void)close(executable);
    return 125;
  }
  if (strcmp(arguments[index], "--probe-child") == 0) {
    const int result = argument_count == index + 6 &&
                       ct_trampoline_projection(arguments + index + 1) == 0
                            ? 0
                            : 125;
    (void)close(executable);
    return result;
  }
  if (strcmp(arguments[index], "--probe") == 0) {
    pid_t child;
    int status;
    char executable_path[64];
    char control[32];
    char inherited[32];
    char *child_arguments[13];
    if (argument_count != index + 6 ||
        ct_trampoline_projection(arguments + index + 1) != 0 ||
        snprintf(executable_path, sizeof(executable_path), "/proc/self/fd/%d",
                 executable) < 0 ||
        snprintf(control, sizeof(control), "%d", control_descriptor) < 0 ||
        (inherited_descriptor >= 3 &&
         snprintf(inherited, sizeof(inherited), "%d", inherited_descriptor) < 0)) {
      (void)close(executable);
      return 125;
    }
    child_arguments[0] = executable_path;
    child_arguments[1] = "--internal-trampoline";
    child_arguments[2] = "--control-fd";
    child_arguments[3] = control;
    if (inherited_descriptor >= 3) {
      child_arguments[4] = "--descriptor-fd";
      child_arguments[5] = inherited;
      child_arguments[6] = "--probe-child";
      child_arguments[7] = arguments[index + 1];
      child_arguments[8] = arguments[index + 2];
      child_arguments[9] = arguments[index + 3];
      child_arguments[10] = arguments[index + 4];
      child_arguments[11] = arguments[index + 5];
      child_arguments[12] = NULL;
    } else {
      child_arguments[4] = "--probe-child";
      child_arguments[5] = arguments[index + 1];
      child_arguments[6] = arguments[index + 2];
      child_arguments[7] = arguments[index + 3];
      child_arguments[8] = arguments[index + 4];
      child_arguments[9] = arguments[index + 5];
      child_arguments[10] = NULL;
    }
    child = fork();
    if (child == 0) {
      execv(executable_path, child_arguments);
      _exit(125);
    }
    if (child < 0 || waitpid(child, &status, 0) != child || close(executable) != 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 125;
    return 0;
  }
  if (arguments[index][0] != '/' || close(executable) != 0) return 125;
  execv(arguments[index], arguments + index);
  return errno == ENOENT ? 127 : 126;
}
