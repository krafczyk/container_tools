/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "supervisor.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void pause_milliseconds(long milliseconds)
{
  const struct timespec pause = {milliseconds / 1000L,
                                 (milliseconds % 1000L) * 1000000L};
  (void)nanosleep(&pause, NULL);
}

static long long now_milliseconds(void)
{
  struct timespec value;
  return clock_gettime(CLOCK_MONOTONIC, &value) == 0
             ? (long long)value.tv_sec * 1000LL + value.tv_nsec / 1000000L
             : -1LL;
}

struct thread_descendant {
  int descriptor;
  int ready;
};

static void *start_thread_descendant(void *data)
{
  const struct thread_descendant *fixture = data;
  const pid_t descendant = fork();
  if (descendant < 0) return NULL;
  if (descendant == 0) {
    const pid_t identity = getpid();
    if (setsid() < 0 || signal(SIGTERM, SIG_IGN) == SIG_ERR ||
        write(fixture->descriptor, &identity, sizeof(identity)) !=
            (ssize_t)sizeof(identity) || write(fixture->ready, "x", 1U) != 1) {
      _exit(125);
    }
    pause_milliseconds(5000L);
    _exit(0);
  }
  pause_milliseconds(5000L);
  return NULL;
}

int main(int argument_count, char **arguments)
{
  char *const true_command[] = {"/bin/true", NULL};
  char *status_command[] = {"/proc/self/exe", "--status", "42", NULL};
  char *status_125_command[] = {"/proc/self/exe", "--status", "125", NULL};
  char *status_255_command[] = {"/proc/self/exe", "--status", "255", NULL};
  char *signal_command[] = {"/proc/self/exe", "--signal", NULL};
  char *leader_command[] = {"/proc/self/exe", "--leader", NULL};
  char *stubborn_command[] = {"/proc/self/exe", "--stubborn", NULL};
  char *mask_command[] = {"/proc/self/exe", "--mask", NULL};
  char *sigchld_ignored_command[] = {"/proc/self/exe",
                                     "--sigchld-ignored", NULL};
  char *late_ready_command[] = {"/proc/self/exe", "--late-ready", NULL};

  if (argument_count > 1 && strcmp(arguments[1], "--status") == 0) {
    return argument_count == 3 ? atoi(arguments[2]) : 125;
  }
  if (argument_count > 1 && strcmp(arguments[1], "--signal") == 0) {
    (void)signal(SIGUSR1, SIG_DFL);
    return raise(SIGUSR1) == 0 ? 125 : 126;
  }
  if (argument_count > 1 && strcmp(arguments[1], "--mask") == 0) {
    sigset_t mask;
    return sigprocmask(SIG_SETMASK, NULL, &mask) != 0 ||
                   sigismember(&mask, SIGTERM) != 1
               ? 125
               : 0;
  }
  if (argument_count > 1 && strcmp(arguments[1], "--sigchld-ignored") == 0) {
    struct sigaction action;
    return sigaction(SIGCHLD, NULL, &action) != 0 ||
                   action.sa_handler != SIG_IGN
               ? 125
               : 0;
  }
  if (argument_count > 1 && strcmp(arguments[1], "--leader") == 0) {
    const pid_t descendant = fork();
    if (descendant < 0) return 125;
    if (descendant == 0) {
      pause_milliseconds(100L);
      _exit(0);
    }
    return 42;
  }
  if (argument_count > 1 && strcmp(arguments[1], "--stubborn") == 0) {
    const pid_t descendant = fork();
    if (descendant < 0) return 125;
    if (descendant == 0) {
      (void)signal(SIGTERM, SIG_IGN);
      pause_milliseconds(5000L);
      _exit(0);
    }
    return 0;
  }
  if (argument_count > 1 && strcmp(arguments[1], "--late-ready") == 0) {
    (void)signal(SIGTERM, SIG_IGN);
    pause_milliseconds(5000L);
    return 0;
  }
  if (argument_count > 1 &&
      strcmp(arguments[1], "--force-thread-supervisor-failure") == 0) {
    const int descriptor = argument_count == 3 ? atoi(arguments[2]) : -1;
    int prepared[2];
    char ready;
    pthread_t thread;
    struct thread_descendant fixture;
    if (descriptor < 0 || pipe(prepared) != 0 ||
        signal(SIGTERM, SIG_IGN) == SIG_ERR) return 125;
    fixture.descriptor = descriptor;
    fixture.ready = prepared[1];
    if (pthread_create(&thread, NULL, start_thread_descendant, &fixture) != 0 ||
        read(prepared[0], &ready, 1U) != 1 || close(prepared[0]) != 0 ||
        close(prepared[1]) != 0) return 125;
    pause_milliseconds(5000L);
    return 0;
  }
  if (argument_count > 1 &&
      (strcmp(arguments[1], "--escaped") == 0 ||
       strcmp(arguments[1], "--force-supervisor-failure") == 0 ||
       strcmp(arguments[1], "--force-partial-collection") == 0 ||
       strcmp(arguments[1], "--force-no-pidfd") == 0)) {
    const int descriptor = argument_count == 3 ? atoi(arguments[2]) : -1;
    int prepared[2];
    char ready;
    if (descriptor < 0 || pipe(prepared) != 0) return 125;
    const pid_t descendant = fork();
    if (descendant < 0) return 125;
    if (descendant == 0) {
      const pid_t identity = getpid();
      if (close(prepared[0]) != 0 || setsid() < 0 ||
          signal(SIGTERM, SIG_IGN) == SIG_ERR ||
          write(descriptor, &identity, sizeof(identity)) !=
              (ssize_t)sizeof(identity) || write(prepared[1], "x", 1U) != 1)
        _exit(125);
      pause_milliseconds(5000L);
      _exit(0);
    }
    return close(prepared[1]) != 0 || read(prepared[0], &ready, 1U) != 1 ||
                   close(prepared[0]) != 0
               ? 125
               : 0;
  }
  if (argument_count > 1 && strcmp(arguments[1], "--fd") == 0) {
    const int descriptor = argument_count == 3 ? atoi(arguments[2]) : -1;
    return descriptor < 0 || write(descriptor, "x", 1U) != 1 ? 125 : 0;
  }
  if (ct_supervisor_probe(true_command, 500U) != 0) return 11;
  if (ct_supervisor_probe(status_command, 500U) != 42) return 12;
  if (ct_supervisor_probe(status_125_command, 500U) != 125) return 13;
  {
    struct ct_supervisor_probe_result result;
    if (ct_supervisor_probe_environment_detailed(
            status_125_command, 500U, NULL, 0U, &result) !=
            CT_SUPERVISOR_PROBE_COMPLETED ||
        result.status != 125 || result.infrastructure_failed != 0 ||
        result.interrupted != 0) return 72;
  }
  if (ct_supervisor_probe(status_255_command, 500U) != 255) return 14;
  if (ct_supervisor_probe(signal_command, 500U) != 128 + SIGUSR1) return 15;
  if (ct_supervisor_probe(leader_command, 500U) != 42) return 16;
  if (ct_supervisor_probe(stubborn_command, 20U) != 124) return 17;
  {
    int descriptors[2];
    char number[32], value;
    char *descriptor_command[] = {"/proc/self/exe", "--fd", number, NULL};
    if (pipe(descriptors) != 0 ||
        snprintf(number, sizeof(number), "%d", descriptors[1]) >=
            (int)sizeof(number) ||
        ct_supervisor_probe(descriptor_command, 500U) != 0 ||
        close(descriptors[1]) != 0 || read(descriptors[0], &value, 1U) != 1 ||
        value != 'x' || close(descriptors[0]) != 0) return 2;
  }
  {
    const pid_t unrelated = fork();
    int status;
    if (unrelated < 0) return 3;
    if (unrelated == 0) {
      pause_milliseconds(5000L);
      _exit(0);
    }
    if (ct_supervisor_probe(stubborn_command, 20U) != 124 ||
        kill(unrelated, 0) != 0 || kill(unrelated, SIGTERM) != 0 ||
        waitpid(unrelated, &status, 0) != unrelated || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGTERM) return 3;
  }
  {
    const pid_t interrupted = fork();
    int status;
    if (interrupted < 0) return 4;
    if (interrupted == 0) {
      _exit(ct_supervisor_probe(stubborn_command, 5000U));
    }
    pause_milliseconds(200L);
    if (kill(interrupted, SIGTERM) != 0 ||
        waitpid(interrupted, &status, 0) != interrupted ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) return 4;
  }
  {
    int descriptors[2];
    char number[32];
    pid_t escaped;
    long long started, finished;
    char *escaped_command[] = {"/proc/self/exe", "--escaped", number, NULL};
    if (pipe(descriptors) != 0 ||
        snprintf(number, sizeof(number), "%d", descriptors[1]) >=
            (int)sizeof(number) ||
        (started = now_milliseconds()) < 0LL ||
        ct_supervisor_probe(escaped_command, 20U) != 124 ||
        (finished = now_milliseconds()) < 0LL || finished - started > 2000LL ||
        close(descriptors[1]) != 0 ||
        read(descriptors[0], &escaped, sizeof(escaped)) !=
            (ssize_t)sizeof(escaped) ||
        close(descriptors[0]) != 0) return 5;
    errno = 0;
    if (kill(escaped, 0) == 0 || errno != ESRCH) return 5;
  }
  {
    struct sigaction action, original, restored;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGCHLD, NULL, &original) != 0 ||
        sigaction(SIGCHLD, &action, NULL) != 0) return 60;
    if (ct_supervisor_probe(sigchld_ignored_command, 500U) != 0) return 61;
    if (sigaction(SIGCHLD, NULL, &restored) != 0 ||
        restored.sa_handler != SIG_IGN) return 62;
    action.sa_handler = SIG_DFL;
    action.sa_flags = SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &action, NULL) != 0) return 63;
    if (ct_supervisor_probe(true_command, 500U) != 0) return 64;
    if (sigaction(SIGCHLD, NULL, &restored) != 0 ||
        (restored.sa_flags & SA_NOCLDWAIT) == 0) return 65;
    if (sigaction(SIGCHLD, &original, NULL) != 0) return 66;
  }
  {
    sigset_t blocked, original, restored;
    if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGTERM) != 0 ||
        sigprocmask(SIG_BLOCK, &blocked, &original) != 0 ||
        ct_supervisor_probe(mask_command, 500U) != 0 ||
        ct_supervisor_probe(stubborn_command, 20U) != 124 ||
        sigprocmask(SIG_SETMASK, NULL, &restored) != 0 ||
        sigismember(&restored, SIGTERM) != 1 ||
        sigprocmask(SIG_SETMASK, &original, NULL) != 0) return 7;
  }
  {
    const long long started = now_milliseconds();
    long long finished;
    if (started < 0LL || ct_supervisor_probe(late_ready_command, 20U) != 124 ||
        (finished = now_milliseconds()) < 0LL || finished - started > 2000LL) {
      return 8;
    }
  }
  {
    int descriptors[2];
    char number[32];
    pid_t escaped;
    int status;
    const pid_t unrelated = fork();
    long long started, finished;
    char *failure_command[] = {"/proc/self/exe",
                               "--force-thread-supervisor-failure", number,
                               NULL};
    if (unrelated < 0) return 9;
    if (unrelated == 0) {
      pause_milliseconds(5000L);
      _exit(0);
    }
    struct ct_supervisor_probe_result result;
    if (pipe(descriptors) != 0 ||
        snprintf(number, sizeof(number), "%d", descriptors[1]) >=
            (int)sizeof(number) ||
        (started = now_milliseconds()) < 0LL ||
        ct_supervisor_probe_environment_detailed(
            failure_command, 20U, NULL, 0U, &result) !=
            CT_SUPERVISOR_PROBE_INFRASTRUCTURE_FAILURE ||
        result.status != 125 || result.infrastructure_failed == 0 ||
        result.interrupted != 0 ||
        (finished = now_milliseconds()) < 0LL || finished - started > 2500LL ||
        close(descriptors[1]) != 0 ||
        read(descriptors[0], &escaped, sizeof(escaped)) !=
            (ssize_t)sizeof(escaped) ||
        close(descriptors[0]) != 0) return 9;
    errno = 0;
    if (kill(escaped, 0) == 0 || errno != ESRCH || kill(unrelated, 0) != 0 ||
        kill(unrelated, SIGTERM) != 0 ||
        waitpid(unrelated, &status, 0) != unrelated || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGTERM) return 9;
  }
  {
    int descriptors[2];
    char number[32];
    pid_t escaped;
    long long started, finished;
    char *failure_command[] = {"/proc/self/exe",
                               "--force-partial-collection", number, NULL};
    if (pipe(descriptors) != 0 ||
        snprintf(number, sizeof(number), "%d", descriptors[1]) >=
            (int)sizeof(number) ||
        (started = now_milliseconds()) < 0LL ||
        ct_supervisor_probe(failure_command, 20U) != 125 ||
        (finished = now_milliseconds()) < 0LL || finished - started > 2500LL ||
        close(descriptors[1]) != 0 ||
        read(descriptors[0], &escaped, sizeof(escaped)) !=
            (ssize_t)sizeof(escaped) ||
        close(descriptors[0]) != 0) return 70;
    errno = 0;
    if (kill(escaped, 0) == 0 || errno != ESRCH) return 70;
  }
  {
    int descriptors[2];
    char number[32];
    pid_t escaped;
    int status;
    long long started, finished;
    char *failure_command[] = {"/proc/self/exe", "--force-no-pidfd", number,
                               NULL};
    if (pipe(descriptors) != 0 ||
        snprintf(number, sizeof(number), "%d", descriptors[1]) >=
            (int)sizeof(number) ||
        (started = now_milliseconds()) < 0LL ||
        ct_supervisor_probe(failure_command, 20U) != 125 ||
        (finished = now_milliseconds()) < 0LL || finished - started > 2500LL ||
        close(descriptors[1]) != 0 ||
        read(descriptors[0], &escaped, sizeof(escaped)) !=
            (ssize_t)sizeof(escaped) ||
        close(descriptors[0]) != 0 || kill(escaped, 0) != 0 ||
        kill(escaped, SIGKILL) != 0 ||
        waitpid(escaped, &status, 0) != escaped || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGKILL) return 71;
  }
  errno = 0;
  return 0;
}
