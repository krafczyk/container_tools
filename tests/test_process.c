/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
  char *const command[] = {"/bin/true", NULL};
  const char *const invalid[] = {"nan", "NaN", "inf", "-inf", "1x", "0", NULL};
  char *const noisy[] = {"/bin/sh", "-c",
                         "printf control-out; printf control-err >&2", NULL};
  char captured[64];
  char stdout_path[] = "/tmp/mkchad-v1/container-tools-c11/process-stdout.XXXXXX";
  char stderr_path[] = "/tmp/mkchad-v1/container-tools-c11/process-stderr.XXXXXX";
  int stdout_file, stderr_file, saved_stdout, saved_stderr;
  struct stat stdout_status, stderr_status;
  size_t index;

  for (index = 0U; invalid[index] != NULL; ++index) {
    if (setenv("CT_RUNTIME_OPERATION_TIMEOUT", invalid[index], 1) != 0 ||
        ct_process_run_operation(command, "probe") != 125) return 1;
  }
  if (setenv("CT_RUNTIME_OPERATION_TIMEOUT", "0.05", 1) != 0) return 1;
  {
    char *const ignoring[] = {"/bin/sh", "-c", "trap '' TERM; (trap '' TERM; while :; do sleep 1; done) & wait", NULL};
    if (ct_process_run_operation(ignoring, "probe") != 124) return 1;
  }
  if (unsetenv("CT_RUNTIME_OPERATION_TIMEOUT") != 0) return 1;
  if (ct_process_run_operation(command, "instance-start") != 0 ||
      ct_process_run_operation(command, "instance-probe") != 0) return 1;
  stdout_file = mkstemp(stdout_path);
  stderr_file = mkstemp(stderr_path);
  saved_stdout = dup(STDOUT_FILENO);
  saved_stderr = dup(STDERR_FILENO);
  if (stdout_file < 0 || stderr_file < 0 || saved_stdout < 0 || saved_stderr < 0 ||
      dup2(stdout_file, STDOUT_FILENO) < 0 || dup2(stderr_file, STDERR_FILENO) < 0 ||
      ct_process_run_operation_quiet(noisy, "probe") != 0 ||
      fstat(stdout_file, &stdout_status) != 0 || stdout_status.st_size != 0 ||
      fstat(stderr_file, &stderr_status) != 0 || stderr_status.st_size != 0 ||
      ct_process_run_operation_capture_quiet(noisy, "probe", captured,
                                             sizeof(captured)) != 0 ||
      strcmp(captured, "control-out") != 0 ||
      fstat(stderr_file, &stderr_status) != 0 || stderr_status.st_size != 0 ||
      ct_process_run_operation_stdout_to_stderr(noisy, "probe") != 0 ||
      fstat(stdout_file, &stdout_status) != 0 || stdout_status.st_size != 0 ||
      fstat(stderr_file, &stderr_status) != 0 || stderr_status.st_size == 0 ||
      dup2(saved_stdout, STDOUT_FILENO) < 0 || dup2(saved_stderr, STDERR_FILENO) < 0 ||
      close(saved_stdout) != 0 || close(saved_stderr) != 0 ||
      close(stdout_file) != 0 || close(stderr_file) != 0 ||
      unlink(stdout_path) != 0 || unlink(stderr_path) != 0) return 1;
  {
    const pid_t child = fork();
    int status;
    const struct timespec pause = {0, 200000000L};
    if (child < 0) return 1;
    if (child == 0) {
      char *const slow[] = {"/bin/sh", "-c", "sleep 10", NULL};
      _exit(ct_process_run_operation_quiet(slow, "instance-probe"));
    }
    (void)nanosleep(&pause, NULL);
    if (kill(child, SIGTERM) != 0 || waitpid(child, &status, 0) != child ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) return 1;
  }
  return 0;
}
