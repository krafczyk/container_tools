/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#define _XOPEN_SOURCE 600
#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int ct_test_wait_readable(int descriptor,
                                 const struct timespec *deadline)
{
  struct pollfd input = {descriptor, POLLIN, 0};

  for (;;) {
    struct timespec now;
    long long milliseconds;
    int result;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 1;
    milliseconds = (long long)(deadline->tv_sec - now.tv_sec) * 1000LL +
                   (long long)(deadline->tv_nsec - now.tv_nsec) / 1000000LL;
    if (milliseconds <= 0LL) return 1;
    if (milliseconds > 6000LL) milliseconds = 6000LL;
    result = poll(&input, 1U, (int)milliseconds);
    if (result > 0) return (input.revents & POLLIN) == 0;
    if (result == 0 || errno != EINTR) return 1;
  }
}

static int ct_test_interactive_run(void)
{
  char *slave_name;
  char ready;
  char output[128] = {0};
  size_t output_size = 0U;
  int master = -1, descriptors[2] = {-1, -1}, status, result = 1;
  pid_t child = -1;
  int child_reaped = 0;
  struct timespec deadline;

  master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
      (slave_name = ptsname(master)) == NULL || pipe(descriptors) != 0) {
    goto cleanup;
  }
  child = fork();
  if (child == 0) {
    char *const interactive[] = {
        "/bin/sh", "-c",
        "trap 'exit 0' INT; kill -STOP $$; "
        "IFS= read -r value && [ \"$value\" = ready ] && "
        "printf armed; while :; do sleep 1; done",
        NULL};
    int slave;

    (void)close(descriptors[0]);
    if (setsid() < 0 ||
        (slave = open(slave_name, O_RDWR)) < 0 ||
        ioctl(slave, TIOCSCTTY, 0) != 0 ||
        dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
        dup2(slave, STDERR_FILENO) < 0 ||
        (slave > STDERR_FILENO && close(slave) != 0) ||
        write(descriptors[1], "1", 1U) != 1 || close(descriptors[1]) != 0) {
      _exit(125);
    }
    (void)alarm(5U);
    _exit(ct_process_run(interactive, NULL, 0U));
  }
  (void)close(descriptors[1]);
  descriptors[1] = -1;
  if (child < 0 || read(descriptors[0], &ready, 1U) != 1) goto cleanup;
  if (close(descriptors[0]) != 0) {
    descriptors[0] = -1;
    goto cleanup;
  }
  descriptors[0] = -1;
  if (waitpid(child, &status, WUNTRACED) != child || !WIFSTOPPED(status) ||
      kill(child, SIGCONT) != 0 || write(master, "ready\n", 6U) != 6 ||
      clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
    goto cleanup;
  }
  deadline.tv_sec += 6;
  while (strstr(output, "armed") == NULL) {
    ssize_t received;
    if (output_size + 1U == sizeof(output) ||
        ct_test_wait_readable(master, &deadline) != 0 ||
        (received = read(master, output + output_size,
                         sizeof(output) - output_size - 1U)) <= 0) {
      goto cleanup;
    }
    output_size += (size_t)received;
    output[output_size] = '\0';
  }
  if (write(master, "\003", 1U) != 1 || waitpid(child, &status, 0) != child) {
    goto cleanup;
  }
  child_reaped = 1;
  result = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
cleanup:
  if (descriptors[0] >= 0) (void)close(descriptors[0]);
  if (descriptors[1] >= 0) (void)close(descriptors[1]);
  if (child > 0 && !child_reaped) {
    (void)kill(child, SIGKILL);
    (void)waitpid(child, NULL, 0);
  }
  if (master >= 0 && close(master) != 0) result = 1;
  return result;
}

int main(void)
{
  char *const command[] = {"/bin/true", NULL};
  char *const exit_42[] = {"/bin/sh", "-c", "exit 42", NULL};
  char *const signaled[] = {"/bin/sh", "-c", "kill -TERM $$", NULL};
  const char *const invalid[] = {"nan", "NaN", "inf", "-inf", "1x", "0", NULL};
  char *const noisy[] = {"/bin/sh", "-c",
                         "printf control-out; printf control-err >&2", NULL};
  char captured[64];
  char stdout_path[] = "/tmp/mkchad-v1/container-tools-c11/process-stdout.XXXXXX";
  char stderr_path[] = "/tmp/mkchad-v1/container-tools-c11/process-stderr.XXXXXX";
  int stdout_file, stderr_file, saved_stdout, saved_stderr;
  struct stat stdout_status, stderr_status;
  size_t index;

  if (ct_process_run(command, NULL, 0U) != 0 ||
      ct_process_run(exit_42, NULL, 0U) != 42 ||
      ct_process_run(signaled, NULL, 0U) != 128 + SIGTERM ||
      ct_test_interactive_run() != 0) return 1;
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
