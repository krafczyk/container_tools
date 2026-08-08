/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "executable.h"
#include "package_identity.h"
#include "trampoline.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_probe(int control, int inherited, const char *cwd,
                     const char *target, const struct stat *metadata)
{
  char control_text[32];
  char inherited_text[32];
  char device[32];
  char inode[32];
  char type[32];
  char *arguments[] = {"--internal-trampoline", "--control-fd", control_text,
                        "--descriptor-fd", inherited_text, "--probe", (char *)cwd,
                        (char *)target, device, inode, type, NULL};
  pid_t child;
  int status;
  if (snprintf(control_text, sizeof(control_text), "%d", control) < 0 ||
      snprintf(inherited_text, sizeof(inherited_text), "%d", inherited) < 0 ||
      snprintf(device, sizeof(device), "%ju", (uintmax_t)metadata->st_dev) < 0 ||
      snprintf(inode, sizeof(inode), "%ju", (uintmax_t)metadata->st_ino) < 0 ||
      snprintf(type, sizeof(type), "%ju",
               (uintmax_t)(metadata->st_mode & S_IFMT)) < 0) return 1;
  child = fork();
  if (child == 0) _exit(ct_trampoline_main(11, arguments, CT_BUILD_IDENTITY));
  return child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)
             ? 1
             : WEXITSTATUS(status);
}

int main(int argument_count, char **arguments)
{
  int control;
  int inherited;
  struct stat root;
  struct stat mismatch;
  if (argument_count >= 2 && strcmp(arguments[1], "--internal-trampoline") == 0) {
    return ct_trampoline_main(argument_count - 1, arguments + 1, CT_BUILD_IDENTITY);
  }
  control = ct_executable_control_open(CT_BUILD_IDENTITY);
  inherited = fcntl(STDIN_FILENO, F_DUPFD, 200);
  if (control < 0 || inherited < 0 || chdir("/") != 0 || stat("/", &root) != 0) return 1;
  mismatch = root;
  if ((uintmax_t)mismatch.st_ino == UINTMAX_MAX) mismatch.st_ino = (ino_t)0;
  else ++mismatch.st_ino;
  if (run_probe(control, inherited, "/", "/", &root) != 0 ||
      run_probe(control, inherited, "/not-the-selected-root", "/", &root) == 0 ||
      run_probe(control, inherited, "/", "/", &mismatch) == 0) return 2;
  return close(control) != 0 || close(inherited) != 0 ? 3 : 0;
}
