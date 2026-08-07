/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "process.h"

#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t ct_process_child_group;

static void ct_process_forward_signal(int signal_number)
{
  if (ct_process_child_group > 0) {
    (void)kill(-(pid_t)ct_process_child_group, signal_number);
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

int ct_process_run(char *const arguments[],
                   const struct ct_process_environment *environment,
                   size_t environment_count)
{
  pid_t child;
  int status = 0;
  int ready[2] = {-1, -1};
  int release[2] = {-1, -1};
  size_t index;
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
  int wait_result;

  if (arguments == NULL || arguments[0] == NULL || environment_count > 32U ||
      (environment_count != 0U && environment == NULL)) return 64;
  if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGINT) != 0 ||
      sigaddset(&blocked, SIGTERM) != 0 || sigprocmask(SIG_BLOCK, &blocked, &old_mask) != 0) {
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
    for (index = 0U; index < environment_count; ++index) {
      if (environment[index].name == NULL || environment[index].value == NULL ||
          setenv(environment[index].name, environment[index].value, 1) != 0) _exit(125);
    }
    execvp(arguments[0], arguments);
    _exit(errno == ENOENT ? 127 : 126);
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
  if (ct_process_write_byte(release[1], 0) == 0) child_released = true;
  (void)close(release[1]);
  mask_restore_result = sigprocmask(SIG_SETMASK, &old_mask, NULL);
  wait_result = ct_process_wait(child, &status);
  if (!child_released || mask_restore_result != 0 || wait_result != 0) {
    ct_process_child_group = 0;
    if (terminate_installed) (void)sigaction(SIGTERM, &old_terminate, NULL);
    if (interrupt_installed) (void)sigaction(SIGINT, &old_interrupt, NULL);
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  ct_process_child_group = 0;
  if (sigaction(SIGTERM, &old_terminate, NULL) != 0 ||
      sigaction(SIGINT, &old_interrupt, NULL) != 0 ||
      sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) return 125;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 125;
}
