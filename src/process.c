/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t ct_process_child_group;
static volatile sig_atomic_t ct_process_interrupted;

int ct_process_apply_environment(
    const struct ct_process_environment *environment,
    size_t environment_count)
{
  size_t index;
  if (environment_count > CT_PROCESS_ENVIRONMENT_LIMIT ||
      (environment_count != 0U && environment == NULL)) {
    return 1;
  }
  for (index = 0U; index < environment_count; ++index) {
    if (environment[index].name == NULL ||
        (environment[index].value == NULL
             ? unsetenv(environment[index].name)
             : setenv(environment[index].name, environment[index].value, 1)) != 0) {
      return 1;
    }
  }
  return 0;
}

static int ct_process_validate_exec(
    char *const arguments[], const struct ct_process_environment *environment,
    size_t environment_count)
{
  return arguments == NULL || arguments[0] == NULL ||
                 environment_count > CT_PROCESS_ENVIRONMENT_LIMIT ||
                 (environment_count != 0U && environment == NULL)
             ? 64
             : 0;
}

static int ct_process_exec_prepared(
    char *const arguments[], const struct ct_process_environment *environment,
    size_t environment_count)
{
  if (ct_process_apply_environment(environment, environment_count) != 0) {
    return 125;
  }
  execvp(arguments[0], arguments);
  return errno == ENOENT ? 127 : 126;
}

int ct_process_exec(char *const arguments[],
                    const struct ct_process_environment *environment,
                    size_t environment_count)
{
  const int validation =
      ct_process_validate_exec(arguments, environment, environment_count);
  return validation == 0
             ? ct_process_exec_prepared(arguments, environment,
                                        environment_count)
             : validation;
}

static void ct_process_forward_signal(int signal_number)
{
  ct_process_interrupted = signal_number;
  if (ct_process_child_group > 0) {
    (void)kill(-(pid_t)ct_process_child_group, signal_number);
  }
}

static int ct_process_duration(const char *value, long *milliseconds)
{
  char *end;
  double seconds;
  if (value == NULL || value[0] == '\0') return 1;
  errno = 0;
  seconds = strtod(value, &end);
  if (errno != 0 || end == value || *end != '\0' || !isfinite(seconds) || seconds <= 0.0 || seconds > 3600.0 ||
      seconds * 1000.0 > (double)LONG_MAX) return 1;
  *milliseconds = (long)(seconds * 1000.0 + 0.999);
  return *milliseconds <= 0 ? 1 : 0;
}

static int ct_process_operation_timeout(const char *operation, long *milliseconds)
{
  char name[64];
  const char *value;
  size_t index, prefix_length;
  if (operation == NULL) return 1;
  if (strcmp(operation, "instance-probe") == 0 ||
      strcmp(operation, "instance-start") == 0) {
    const int probe = strcmp(operation, "instance-probe") == 0;
    value = getenv(probe ? "CT_INSTANCE_PROBE_TIMEOUT" :
                           "CT_INSTANCE_START_TIMEOUT");
    return ct_process_duration(value == NULL || value[0] == '\0' ?
                                   (probe ? "5" : "30") : value,
                               milliseconds);
  }
  if (strcmp(operation, "probe") != 0 && strcmp(operation, "create") != 0 &&
      strcmp(operation, "start") != 0 && strcmp(operation, "cleanup") != 0) return 1;
  if (snprintf(name, sizeof(name), "CT_RUNTIME_") < 0) return 1;
  prefix_length = strlen(name);
  for (index = prefix_length; operation[index - prefix_length] != '\0'; ++index) {
    const char input = operation[index - prefix_length];
    name[index] = input >= 'a' && input <= 'z' ? (char)(input - ('a' - 'A')) : input;
  }
  if (snprintf(name + index, sizeof(name) - index, "_TIMEOUT") < 0) return 1;
  value = getenv(name);
  if (value == NULL || value[0] == '\0') value = getenv("CT_RUNTIME_OPERATION_TIMEOUT");
  return ct_process_duration(value == NULL || value[0] == '\0' ? "10" : value, milliseconds);
}

static int ct_process_group_alive(pid_t group)
{
  if (kill(-group, 0) == 0 || errno == EPERM) return 1;
  return errno == ESRCH ? 0 : -1;
}

static int ct_process_sleep_until(const struct timespec *deadline)
{
  struct timespec now, pause;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 1;
  if (now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) return 1;
  pause.tv_sec = 0;
  pause.tv_nsec = 10000000L;
  if (nanosleep(&pause, NULL) != 0 && errno != EINTR) return 1;
  return 0;
}

static void ct_process_cleanup_deadline(struct timespec *deadline);

static int ct_process_reap_group(pid_t group)
{
  struct timespec deadline;
  int alive = ct_process_group_alive(group);
  if (alive <= 0) return alive == 0 ? 0 : 1;
  (void)kill(-group, SIGTERM);
  ct_process_cleanup_deadline(&deadline);
  while ((alive = ct_process_group_alive(group)) > 0 && ct_process_sleep_until(&deadline) == 0) { }
  if (alive == 0) return 0;
  (void)kill(-group, SIGKILL);
  ct_process_cleanup_deadline(&deadline);
  while ((alive = ct_process_group_alive(group)) > 0 && ct_process_sleep_until(&deadline) == 0) { }
  return alive == 0 ? 0 : 1;
}

static void ct_process_cleanup_deadline(struct timespec *deadline)
{
  if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) {
    deadline->tv_sec = 0;
    deadline->tv_nsec = 0;
    return;
  }
  deadline->tv_nsec += 50000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    ++deadline->tv_sec;
    deadline->tv_nsec -= 1000000000L;
  }
}

static int ct_process_read_byte(int descriptor, char *value)
{
  ssize_t result;

  do {
    result = read(descriptor, value, 1U);
  } while (result < 0 && errno == EINTR);
  return result == 1 ? 0 : 1;
}

static int ct_process_write_byte(int descriptor, char value)
{
  ssize_t result;

  do {
    result = write(descriptor, &value, 1U);
  } while (result < 0 && errno == EINTR);
  return result == 1 ? 0 : 1;
}

static int ct_process_wait(pid_t child, int *status)
{
  pid_t result;

  do {
    result = waitpid(child, status, 0);
  } while (result < 0 && errno == EINTR);
  return result == child ? 0 : 1;
}

static int ct_process_set_foreground_group(pid_t group)
{
  sigset_t blocked, old_mask;
  int result;

  if (group <= 0 || sigemptyset(&blocked) != 0 ||
      sigaddset(&blocked, SIGTTOU) != 0 ||
      sigprocmask(SIG_BLOCK, &blocked, &old_mask) != 0) return 1;
  result = tcsetpgrp(STDIN_FILENO, group);
  if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) return 1;
  return result != 0;
}

static int ct_process_observe(pid_t child, int handoff_terminal,
                              pid_t caller_group)
{
  siginfo_t information;
  int result;

  for (;;) {
    memset(&information, 0, sizeof(information));
    do {
      result = waitid(P_PID, (id_t)child, &information,
                      WEXITED | (handoff_terminal != 0 ? WSTOPPED : 0) |
                          WNOWAIT);
    } while (result != 0 && errno == EINTR);
    if (result != 0 || information.si_pid != child) return 1;
    if (information.si_code != CLD_STOPPED) return 0;
    do {
      result = waitid(P_PID, (id_t)child, &information, WSTOPPED | WNOHANG);
    } while (result != 0 && errno == EINTR);
    if (result != 0) return 1;
    if (information.si_pid == 0) continue;
    if (tcgetpgrp(STDIN_FILENO) == child &&
        ct_process_set_foreground_group(caller_group) != 0) return 1;
    if (kill(0, SIGSTOP) != 0) return 1;
    if (tcgetpgrp(STDIN_FILENO) == caller_group &&
        ct_process_set_foreground_group(child) != 0) return 1;
    if (kill(-child, SIGCONT) != 0 && errno != ESRCH) return 1;
  }
}

int ct_process_run(char *const arguments[],
                   const struct ct_process_environment *environment,
                   size_t environment_count)
{
  pid_t child;
  int status = 0;
  int ready[2] = {-1, -1};
  int release[2] = {-1, -1};
  struct sigaction action;
  struct sigaction old_interrupt;
  struct sigaction old_terminate;
  sigset_t blocked;
  sigset_t old_mask;
  char group_ready;
  bool interrupt_installed = false;
  bool terminate_installed = false;
  bool child_released = false;
  int mask_restore_result;
  int observe_result;
  int mask_block_result;
  int wait_result;
  const pid_t caller_group = getpgrp();
  const bool handoff_terminal =
      caller_group > 0 && isatty(STDIN_FILENO) != 0 &&
      tcgetpgrp(STDIN_FILENO) == caller_group;
  int terminal_handoff_result = 0;
  int terminal_restore_result = 0;

  if (ct_process_validate_exec(arguments, environment, environment_count) != 0) {
    return 64;
  }
  if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGINT) != 0 ||
      sigaddset(&blocked, SIGTERM) != 0 || sigaddset(&blocked, SIGTTOU) != 0 ||
      sigprocmask(SIG_BLOCK, &blocked, &old_mask) != 0) {
    return 125;
  }
  if (pipe(ready) != 0 || pipe(release) != 0) {
    (void)close(ready[0]);
    (void)close(ready[1]);
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  child = fork();
  if (child < 0) {
    (void)close(ready[0]);
    (void)close(ready[1]);
    (void)close(release[0]);
    (void)close(release[1]);
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  if (child == 0) {
    char proceed;
    const char setup_status = setpgid(0, 0) == 0 ? 0 : 1;

    ct_process_child_group = 0;
    (void)close(ready[0]);
    (void)close(release[1]);
    if (ct_process_write_byte(ready[1], setup_status) != 0 || close(ready[1]) != 0 ||
        setup_status != 0 || ct_process_read_byte(release[0], &proceed) != 0 ||
        close(release[0]) != 0 || sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) {
      _exit(125);
    }
    _exit(ct_process_exec_prepared(arguments, environment, environment_count));
  }
  (void)close(ready[1]);
  (void)close(release[0]);
  if (ct_process_read_byte(ready[0], &group_ready) != 0 || close(ready[0]) != 0 ||
      group_ready != 0) {
    (void)close(release[1]);
    (void)ct_process_wait(child, &status);
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  memset(&action, 0, sizeof(action));
  action.sa_handler = ct_process_forward_signal;
  if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, &old_interrupt) != 0) {
    (void)close(release[1]);
    (void)ct_process_wait(child, &status);
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  interrupt_installed = true;
  if (sigaction(SIGTERM, &action, &old_terminate) != 0) {
    (void)close(release[1]);
    (void)ct_process_wait(child, &status);
    (void)sigaction(SIGINT, &old_interrupt, NULL);
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  terminate_installed = true;
  ct_process_child_group = (sig_atomic_t)child;
  if (handoff_terminal && tcsetpgrp(STDIN_FILENO, child) != 0) {
    terminal_handoff_result = 1;
  }
  if (terminal_handoff_result == 0 &&
      ct_process_write_byte(release[1], 0) == 0) child_released = true;
  (void)close(release[1]);
  mask_restore_result = sigprocmask(SIG_SETMASK, &old_mask, NULL);
  observe_result = ct_process_observe(child, handoff_terminal, caller_group);
  mask_block_result = sigprocmask(SIG_BLOCK, &blocked, NULL);
  if (handoff_terminal && tcsetpgrp(STDIN_FILENO, caller_group) != 0) {
    terminal_restore_result = 1;
  }
  /* WNOWAIT keeps the PID/PGID non-reusable until the handler target is clear. */
  ct_process_child_group = 0;
  wait_result = ct_process_wait(child, &status);
  if (!child_released || mask_restore_result != 0 || observe_result != 0 ||
      mask_block_result != 0 || terminal_handoff_result != 0 ||
      terminal_restore_result != 0 || wait_result != 0) {
    if (terminate_installed) (void)sigaction(SIGTERM, &old_terminate, NULL);
    if (interrupt_installed) (void)sigaction(SIGINT, &old_interrupt, NULL);
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  if (sigaction(SIGTERM, &old_terminate, NULL) != 0 ||
      sigaction(SIGINT, &old_interrupt, NULL) != 0 ||
      sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) return 125;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 125;
}

enum ct_process_output {
  CT_PROCESS_OUTPUT_INHERIT,
  CT_PROCESS_OUTPUT_QUIET,
  CT_PROCESS_OUTPUT_STDOUT_TO_STDERR,
  CT_PROCESS_OUTPUT_CAPTURE,
  CT_PROCESS_OUTPUT_CAPTURE_QUIET
};

static int ct_process_run_operation_internal(char *const arguments[],
                                             const char *operation,
                                             enum ct_process_output output,
                                             int capture_output)
{
  pid_t child;
  int ready[2] = {-1, -1};
  int status = 0;
  char group_ready;
  long milliseconds;
  struct timespec deadline;
  struct sigaction action, old_interrupt, old_terminate;
  sigset_t blocked, old_mask;
  int timed_out = 0;
  int child_done = 0;
  int cleanup_failed = 0;
  int restore_failed = 0;
  int interrupted = 0;
  struct timespec cleanup_deadline;

  if (arguments == NULL || arguments[0] == NULL || ct_process_operation_timeout(operation, &milliseconds) != 0 ||
      clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return 125;
  deadline.tv_sec += milliseconds / 1000L;
  deadline.tv_nsec += (milliseconds % 1000L) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
  if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGINT) != 0 || sigaddset(&blocked, SIGTERM) != 0 ||
      sigprocmask(SIG_BLOCK, &blocked, &old_mask) != 0) return 125;
  if (pipe(ready) != 0) {
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  child = fork();
  if (child == 0) {
    const char setup_status = setpgid(0, 0) == 0 ? 0 : 1;
    int null_descriptor = -1;
    (void)close(ready[0]);
    if (output == CT_PROCESS_OUTPUT_QUIET ||
        output == CT_PROCESS_OUTPUT_CAPTURE_QUIET) {
      null_descriptor = open("/dev/null", O_WRONLY | O_CLOEXEC);
    }
    if (ct_process_write_byte(ready[1], setup_status) != 0 || close(ready[1]) != 0 || setup_status != 0 ||
        ((output == CT_PROCESS_OUTPUT_CAPTURE ||
          output == CT_PROCESS_OUTPUT_CAPTURE_QUIET) &&
         (dup2(capture_output, STDOUT_FILENO) < 0 || close(capture_output) != 0)) ||
        (output == CT_PROCESS_OUTPUT_STDOUT_TO_STDERR &&
         dup2(STDERR_FILENO, STDOUT_FILENO) < 0) ||
        ((output == CT_PROCESS_OUTPUT_QUIET ||
          output == CT_PROCESS_OUTPUT_CAPTURE_QUIET) &&
         (null_descriptor < 0 ||
          (output == CT_PROCESS_OUTPUT_QUIET &&
           dup2(null_descriptor, STDOUT_FILENO) < 0) ||
          dup2(null_descriptor, STDERR_FILENO) < 0 ||
          close(null_descriptor) != 0)) ||
        sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) _exit(125);
    execvp(arguments[0], arguments);
    _exit(errno == ENOENT ? 127 : 126);
  }
  if (child < 0) { (void)close(ready[0]); (void)close(ready[1]); (void)sigprocmask(SIG_SETMASK, &old_mask, NULL); return 125; }
  (void)close(ready[1]);
  if (ct_process_read_byte(ready[0], &group_ready) != 0 || close(ready[0]) != 0 || group_ready != 0) {
    (void)kill(-child, SIGKILL); (void)ct_process_wait(child, &status); (void)sigprocmask(SIG_SETMASK, &old_mask, NULL); return 125;
  }
  memset(&action, 0, sizeof(action));
  action.sa_handler = ct_process_forward_signal;
  if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, &old_interrupt) != 0) {
    (void)kill(-child, SIGKILL); (void)ct_process_wait(child, &status); (void)sigprocmask(SIG_SETMASK, &old_mask, NULL); return 125;
  }
  if (sigaction(SIGTERM, &action, &old_terminate) != 0) {
    (void)kill(-child, SIGKILL); (void)ct_process_wait(child, &status);
    (void)sigaction(SIGINT, &old_interrupt, NULL);
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  ct_process_child_group = (sig_atomic_t)child;
  ct_process_interrupted = 0;
  if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) {
    (void)kill(-child, SIGKILL);
    (void)ct_process_wait(child, &status);
    (void)sigaction(SIGTERM, &old_terminate, NULL);
    (void)sigaction(SIGINT, &old_interrupt, NULL);
    return 125;
  }
  while (!child_done) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) child_done = 1;
    else if (waited < 0 && errno != EINTR) timed_out = 1;
    if (!child_done && ct_process_interrupted != 0) interrupted = (int)ct_process_interrupted;
    if (!child_done && interrupted == 0 && ct_process_sleep_until(&deadline) != 0) timed_out = 1;
    if (timed_out || interrupted != 0) {
      (void)kill(-child, interrupted != 0 ? interrupted : SIGTERM);
      break;
    }
  }
  if (ct_process_reap_group(child) != 0) {
    cleanup_failed = 1;
  }
  ct_process_cleanup_deadline(&cleanup_deadline);
  while (!child_done && ct_process_sleep_until(&cleanup_deadline) == 0) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) child_done = 1;
    else if (waited < 0 && errno != EINTR) { cleanup_failed = 1; break; }
  }
  ct_process_child_group = 0;
  if (sigaction(SIGTERM, &old_terminate, NULL) != 0) restore_failed = 1;
  if (sigaction(SIGINT, &old_interrupt, NULL) != 0) restore_failed = 1;
  if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) restore_failed = 1;
  if (restore_failed) return 125;
  if (interrupted != 0) {
    ct_process_interrupted = 0;
    if (raise(interrupted) != 0) return 125;
    return 128 + interrupted;
  }
  if (timed_out) return 124;
  if (!child_done || cleanup_failed) return 125;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 125;
}

int ct_process_run_operation(char *const arguments[], const char *operation)
{
  return ct_process_run_operation_internal(arguments, operation,
                                           CT_PROCESS_OUTPUT_INHERIT, -1);
}

int ct_process_run_operation_quiet(char *const arguments[], const char *operation)
{
  return ct_process_run_operation_internal(arguments, operation,
                                           CT_PROCESS_OUTPUT_QUIET, -1);
}

int ct_process_run_operation_stdout_to_stderr(char *const arguments[],
                                              const char *operation)
{
  return ct_process_run_operation_internal(
      arguments, operation, CT_PROCESS_OUTPUT_STDOUT_TO_STDERR, -1);
}

static int ct_process_run_operation_capture_internal(
    char *const arguments[], const char *operation, char output[],
    size_t output_size, enum ct_process_output output_policy)
{
  int descriptors[2] = {-1, -1};
  int result;
  ssize_t received;
  size_t used = 0U;
  if (output == NULL || output_size < 2U || pipe(descriptors) != 0) return 125;
  result = ct_process_run_operation_internal(arguments, operation, output_policy,
                                             descriptors[1]);
  if (close(descriptors[1]) != 0) result = 125;
  while (used + 1U < output_size && (received = read(descriptors[0], output + used, output_size - used - 1U)) > 0) used += (size_t)received;
  if (received < 0 || close(descriptors[0]) != 0 || (received > 0 && used + 1U == output_size)) return 125;
  output[used] = '\0';
  return result;
}

int ct_process_run_operation_capture(char *const arguments[], const char *operation,
                                     char output[], size_t output_size)
{
  return ct_process_run_operation_capture_internal(
      arguments, operation, output, output_size, CT_PROCESS_OUTPUT_CAPTURE);
}

int ct_process_run_operation_capture_quiet(char *const arguments[],
                                           const char *operation, char output[],
                                           size_t output_size)
{
  return ct_process_run_operation_capture_internal(
      arguments, operation, output, output_size,
      CT_PROCESS_OUTPUT_CAPTURE_QUIET);
}
