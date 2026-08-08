/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "supervisor.h"

#include "supervisor_process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CT_SUPERVISOR_GRACE_MILLISECONDS 500LL
#define CT_SUPERVISOR_FORCE_MILLISECONDS 500LL
#define CT_SUPERVISOR_PARENT_CLEANUP_MILLISECONDS 1200LL
#define CT_SUPERVISOR_POLL_MILLISECONDS 10LL

static volatile sig_atomic_t ct_supervisor_group;
static volatile sig_atomic_t ct_supervisor_interrupted;
static volatile sig_atomic_t ct_supervisor_cleanup;

static int ct_supervisor_status(int status);

static void ct_supervisor_forward(int signal_number)
{
  ct_supervisor_interrupted = signal_number;
  if (ct_supervisor_group > 0) {
    (void)kill(-(pid_t)ct_supervisor_group, signal_number);
  }
}

static void ct_supervisor_hold(int signal_number)
{
  ct_supervisor_cleanup = signal_number;
}

static long long ct_supervisor_now(void)
{
  return ct_supervisor_process_now();
}

static int ct_supervisor_pause(long long deadline)
{
  return ct_supervisor_process_pause(deadline);
}

static int ct_supervisor_nonblocking(int descriptor)
{
  const int flags = fcntl(descriptor, F_GETFL);
  return flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0;
}

static int ct_supervisor_await_start(int descriptor, long long deadline)
{
  char start;
  for (;;) {
    const ssize_t count = read(descriptor, &start, 1U);
    if (count == 1) return close(descriptor) != 0;
    if (count == 0) {
      (void)close(descriptor);
      return 1;
    }
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      (void)close(descriptor);
      return 1;
    }
    if (ct_supervisor_now() >= deadline ||
        ct_supervisor_pause(deadline) != 0) {
      (void)close(descriptor);
      return 1;
    }
  }
}

static int ct_supervisor_reap_children(pid_t probe, int *probe_status,
                                       bool *probe_reaped,
                                       bool *children_remain)
{
  for (;;) {
    int status;
    const pid_t reaped = waitpid(-1, &status, WNOHANG);
    if (reaped > 0) {
      if (reaped == probe) {
        *probe_status = ct_supervisor_status(status);
        *probe_reaped = true;
      }
      continue;
    }
    if (reaped == 0) {
      *children_remain = true;
      return 0;
    }
    if (errno == EINTR) continue;
    if (errno == ECHILD) {
      *children_remain = false;
      return 0;
    }
    return 1;
  }
}

static int ct_supervisor_cleanup_children(pid_t probe, int *probe_status,
                                          bool *probe_reaped)
{
  long long deadline = ct_supervisor_now();
  bool children_remain = true;
  if (deadline < 0LL) return 1;
  deadline += CT_SUPERVISOR_GRACE_MILLISECONDS;
  while (children_remain) {
    if (ct_supervisor_process_signal_children(
            getpid(), (int)ct_supervisor_cleanup, deadline) != 0 ||
        ct_supervisor_reap_children(probe, probe_status, probe_reaped,
                                    &children_remain) != 0) return 1;
    if (!children_remain) return 0;
    if (ct_supervisor_now() >= deadline) break;
    if (ct_supervisor_pause(deadline) != 0) return 1;
  }
  deadline = ct_supervisor_now();
  if (deadline < 0LL) return 1;
  deadline += CT_SUPERVISOR_FORCE_MILLISECONDS;
  while (children_remain) {
    if (ct_supervisor_process_signal_children(getpid(), SIGKILL, deadline) !=
            0 ||
        ct_supervisor_reap_children(probe, probe_status, probe_reaped,
                                    &children_remain) != 0) return 1;
    if (!children_remain) return 0;
    if (ct_supervisor_now() >= deadline ||
        ct_supervisor_pause(deadline) != 0) return 1;
  }
  return 0;
}

static _Noreturn void ct_supervisor_child_exit(int report, int status,
                                               bool success)
{
  const char result = success ? 'S' : 'F';
  ssize_t written;
  do {
    written = write(report, &result, 1U);
  } while (written < 0 && errno == EINTR);
  (void)close(report);
  _exit(written == 1 ? status : 125);
}

static _Noreturn void ct_supervisor_child_fail(int report, pid_t probe,
                                               int *probe_status,
                                               bool *probe_reaped)
{
  ct_supervisor_cleanup = SIGTERM;
  if (ct_supervisor_cleanup_children(probe, probe_status, probe_reaped) != 0) {
    for (;;) {
      const struct timespec pause = {0, 10000000L};
      (void)nanosleep(&pause, NULL);
    }
  }
  ct_supervisor_child_exit(report, 125, false);
}

static void ct_supervisor_child(char *const arguments[], int start_gate,
                                int ready, int report,
                                const sigset_t *parent_mask,
                                const struct sigaction *parent_sigchld,
                                long long deadline)
{
  struct sigaction action;
  sigset_t supervisor_mask = *parent_mask;
  pid_t probe;
  int probe_status = 125;
  bool probe_reaped = false;
  char prepared = 0;

  ct_supervisor_cleanup = 0;
  if (setpgid(0, 0) != 0 || prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
    _exit(125);
  }
  memset(&action, 0, sizeof(action));
  action.sa_handler = ct_supervisor_hold;
  if (sigemptyset(&action.sa_mask) != 0 ||
      sigaction(SIGINT, &action, NULL) != 0 ||
      sigaction(SIGTERM, &action, NULL) != 0) _exit(125);
  if (ct_supervisor_await_start(start_gate, deadline) != 0) _exit(125);
  if (sigdelset(&supervisor_mask, SIGINT) != 0 ||
      sigdelset(&supervisor_mask, SIGTERM) != 0) {
    ct_supervisor_child_exit(report, 125, false);
  }
  probe = fork();
  if (probe < 0) ct_supervisor_child_exit(report, 125, false);
  if (probe == 0) {
    struct sigaction ordinary;
    memset(&ordinary, 0, sizeof(ordinary));
    ordinary.sa_handler = SIG_DFL;
    if (sigemptyset(&ordinary.sa_mask) != 0 ||
        sigaction(SIGINT, &ordinary, NULL) != 0 ||
        sigaction(SIGTERM, &ordinary, NULL) != 0 ||
        sigaction(SIGCHLD, parent_sigchld, NULL) != 0 || close(ready) != 0 ||
        close(report) != 0 ||
        sigprocmask(SIG_SETMASK, parent_mask, NULL) != 0) _exit(125);
    execvp(arguments[0], arguments);
    _exit(errno == ENOENT ? 127 : 126);
  }
#ifdef CT_SUPERVISOR_TEST_SEAM
  if (arguments[1] != NULL && strcmp(arguments[1], "--late-ready") == 0) {
    long long delay_deadline = ct_supervisor_now();
    if (delay_deadline < 0LL) {
      ct_supervisor_cleanup = SIGTERM;
    } else {
      delay_deadline += 100LL;
      while (ct_supervisor_now() < delay_deadline) {
        if (ct_supervisor_pause(delay_deadline) != 0) {
          ct_supervisor_cleanup = SIGTERM;
          break;
        }
      }
    }
  }
#endif
  if (write(ready, &prepared, 1U) != 1 || close(ready) != 0 ||
      sigprocmask(SIG_SETMASK, &supervisor_mask, NULL) != 0) {
    ct_supervisor_child_fail(report, probe, &probe_status, &probe_reaped);
  }
  for (;;) {
    bool children_remain;
    if (ct_supervisor_reap_children(probe, &probe_status, &probe_reaped,
                                    &children_remain) != 0) {
      ct_supervisor_child_fail(report, probe, &probe_status, &probe_reaped);
    }
    if (!children_remain) {
      ct_supervisor_child_exit(report, probe_reaped ? probe_status : 125,
                               probe_reaped);
    }
    if (ct_supervisor_cleanup != 0) {
#ifdef CT_SUPERVISOR_TEST_SEAM
      if (arguments[1] != NULL &&
          (strcmp(arguments[1], "--force-supervisor-failure") == 0 ||
           strcmp(arguments[1], "--force-thread-supervisor-failure") == 0 ||
           strcmp(arguments[1], "--force-partial-collection") == 0 ||
           strcmp(arguments[1], "--force-no-pidfd") == 0)) {
        const long long now = ct_supervisor_now();
        if (now < 0LL ||
            ct_supervisor_pause(now + CT_SUPERVISOR_POLL_MILLISECONDS) != 0) {
          ct_supervisor_child_fail(report, probe, &probe_status,
                                   &probe_reaped);
        }
        continue;
      }
#endif
      const int cleanup_status =
          ct_supervisor_cleanup_children(probe, &probe_status, &probe_reaped);
      if (cleanup_status != 0) {
        ct_supervisor_child_fail(report, probe, &probe_status, &probe_reaped);
      }
      ct_supervisor_child_exit(report, 0, true);
    }
    {
      const long long now = ct_supervisor_now();
      if (now < 0LL ||
          ct_supervisor_pause(now + CT_SUPERVISOR_POLL_MILLISECONDS) != 0) {
        ct_supervisor_child_fail(report, probe, &probe_status, &probe_reaped);
      }
    }
  }
}

static int ct_supervisor_observe(pid_t supervisor, int *status, bool *done)
{
  siginfo_t information;
  memset(&information, 0, sizeof(information));
  if (waitid(P_PID, (id_t)supervisor, &information,
             WEXITED | WNOHANG | WNOWAIT) != 0) {
    if (errno == EINTR) return 0;
    return 1;
  }
  if (information.si_pid == 0) return 0;
  /* WNOWAIT keeps the PID/PGID non-reusable until the handler target is clear. */
  ct_supervisor_group = 0;
  for (;;) {
    const pid_t reaped = waitpid(supervisor, status, WNOHANG);
    if (reaped == supervisor) {
      *done = true;
      return 0;
    }
    if (reaped < 0 && errno == EINTR) continue;
    return 1;
  }
}

static int ct_supervisor_wait(pid_t supervisor, int *status, bool *done,
                              long long deadline)
{
  while (!*done) {
    if (ct_supervisor_observe(supervisor, status, done) != 0) return 1;
    if (*done) return 0;
    if (ct_supervisor_now() >= deadline) return 1;
    if (ct_supervisor_pause(deadline) != 0) return 1;
  }
  return 0;
}

static int ct_supervisor_restore(const sigset_t *blocked,
                                 const sigset_t *old_mask,
                                 bool interrupt_installed,
                                 bool terminate_installed,
                                 bool child_normalized,
                                 const struct sigaction *old_interrupt,
                                 const struct sigaction *old_terminate,
                                 const struct sigaction *old_child)
{
  int failed = 0;
  if (sigprocmask(SIG_BLOCK, blocked, NULL) != 0) failed = 1;
  ct_supervisor_group = 0;
  if (terminate_installed &&
      sigaction(SIGTERM, old_terminate, NULL) != 0) failed = 1;
  if (interrupt_installed &&
      sigaction(SIGINT, old_interrupt, NULL) != 0) failed = 1;
  if (child_normalized && sigaction(SIGCHLD, old_child, NULL) != 0) failed = 1;
  if (sigprocmask(SIG_SETMASK, old_mask, NULL) != 0) failed = 1;
  return failed;
}

static int ct_supervisor_status(int status)
{
  return WIFEXITED(status) ? WEXITSTATUS(status)
                           : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
}

int ct_supervisor_probe(char *const arguments[],
                        unsigned int timeout_milliseconds)
{
  pid_t supervisor = -1;
  long long start;
  long long deadline;
  int status = 0;
  int start_gate[2] = {-1, -1};
  int ready[2] = {-1, -1};
  int report[2] = {-1, -1};
  char prepared;
  char result = 0;
  bool ready_received = false;
  bool timed_out = false;
  bool child_done = false;
  bool cleanup_failed = false;
  bool interrupt_installed = false;
  bool terminate_installed = false;
  bool child_normalized = false;
  bool group_established = false;
  bool supervisor_forced = false;
  bool owned_collected = false;
  struct ct_supervisor_process_owned owned;
  struct ct_supervisor_process_options process_options = {false, false};
  struct sigaction action, old_interrupt, old_terminate, old_child;
  sigset_t blocked, old_mask;

  if (arguments == NULL || arguments[0] == NULL ||
      timeout_milliseconds == 0U || timeout_milliseconds > 5000U) return 125;
  ct_supervisor_process_owned_init(&owned);
#ifdef CT_SUPERVISOR_TEST_SEAM
  process_options.force_pidfd_unavailable =
      arguments[1] != NULL &&
      strcmp(arguments[1], "--force-no-pidfd") == 0;
  process_options.fail_after_first_descendant =
      arguments[1] != NULL &&
      strcmp(arguments[1], "--force-partial-collection") == 0;
#endif
  start = ct_supervisor_now();
  if (start < 0LL || prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0 ||
      sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGINT) != 0 ||
      sigaddset(&blocked, SIGTERM) != 0 || sigaddset(&blocked, SIGCHLD) != 0 ||
      sigprocmask(SIG_BLOCK, &blocked, &old_mask) != 0) return 125;
  deadline = start + (long long)timeout_milliseconds;
  if (sigaction(SIGCHLD, NULL, &old_child) != 0) {
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  if (sigemptyset(&action.sa_mask) != 0 ||
      sigaction(SIGCHLD, &action, NULL) != 0) {
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 125;
  }
  child_normalized = true;
  if (pipe(start_gate) != 0 || pipe(ready) != 0 || pipe(report) != 0 ||
      ct_supervisor_nonblocking(start_gate[0]) != 0 ||
      ct_supervisor_nonblocking(ready[0]) != 0 ||
      ct_supervisor_nonblocking(report[0]) != 0) {
    cleanup_failed = true;
    goto finish;
  }
  supervisor = fork();
  if (supervisor == 0) {
    (void)close(start_gate[1]);
    (void)close(ready[0]);
    (void)close(report[0]);
    ct_supervisor_child(arguments, start_gate[0], ready[1], report[1],
                        &old_mask, &old_child, deadline);
  }
  (void)close(ready[1]);
  ready[1] = -1;
  (void)close(report[1]);
  report[1] = -1;
  if (supervisor < 0 || setpgid(supervisor, supervisor) != 0) {
    cleanup_failed = true;
    goto cleanup;
  }
  group_established = true;
  memset(&action, 0, sizeof(action));
  action.sa_handler = ct_supervisor_forward;
  if (sigemptyset(&action.sa_mask) != 0 ||
      sigaction(SIGINT, &action, &old_interrupt) != 0) {
    cleanup_failed = true;
    goto cleanup;
  }
  interrupt_installed = true;
  if (sigaction(SIGTERM, &action, &old_terminate) != 0) {
    cleanup_failed = true;
    goto cleanup;
  }
  terminate_installed = true;
  ct_supervisor_group = (sig_atomic_t)supervisor;
  ct_supervisor_interrupted = 0;
  if (write(start_gate[1], "x", 1U) != 1) {
    cleanup_failed = true;
    goto cleanup;
  }
  if (close(start_gate[1]) != 0) {
    start_gate[1] = -1;
    cleanup_failed = true;
    goto cleanup;
  }
  start_gate[1] = -1;
  if (close(start_gate[0]) != 0) {
    start_gate[0] = -1;
    cleanup_failed = true;
    goto cleanup;
  }
  start_gate[0] = -1;
  if (sigprocmask(SIG_SETMASK, &old_mask, NULL) != 0) {
    cleanup_failed = true;
    goto cleanup;
  }
  while (!ready_received && !child_done && ct_supervisor_interrupted == 0) {
    const ssize_t count = read(ready[0], &prepared, 1U);
    if (count == 1) {
      ready_received = true;
      break;
    }
    if (count < 0 && errno != EINTR && errno != EAGAIN &&
        errno != EWOULDBLOCK) {
      cleanup_failed = true;
      break;
    }
    if (count == 0 ||
        ct_supervisor_observe(supervisor, &status, &child_done) != 0) {
      cleanup_failed = true;
      break;
    }
    if (ct_supervisor_now() >= deadline) {
      timed_out = true;
      break;
    }
    if (ct_supervisor_pause(deadline) != 0) {
      cleanup_failed = true;
      break;
    }
  }
  while (ready_received && !child_done && !timed_out &&
         ct_supervisor_interrupted == 0 && !cleanup_failed) {
    if (ct_supervisor_observe(supervisor, &status, &child_done) != 0) {
      cleanup_failed = true;
      break;
    }
    if (!child_done && ct_supervisor_now() >= deadline) {
      timed_out = true;
      break;
    }
    if (!child_done && ct_supervisor_pause(deadline) != 0) {
      cleanup_failed = true;
    }
  }

cleanup:
  if (!child_done && supervisor > 0) {
    const int cleanup_signal = ct_supervisor_interrupted != 0
                                   ? (int)ct_supervisor_interrupted
                                   : SIGTERM;
    long long cleanup_deadline = ct_supervisor_now();
    if (cleanup_deadline < 0LL) {
      cleanup_failed = true;
    } else {
      if (ct_supervisor_process_signal_anchor(
              supervisor, group_established,
              group_established ? cleanup_signal : SIGKILL, cleanup_deadline,
              &process_options) != 0 &&
          errno != ESRCH) cleanup_failed = true;
      cleanup_deadline += CT_SUPERVISOR_PARENT_CLEANUP_MILLISECONDS;
      if (ct_supervisor_wait(supervisor, &status, &child_done,
                             cleanup_deadline) != 0) {
        cleanup_failed = true;
      }
    }
    if (!child_done) {
      long long force_deadline = ct_supervisor_now();
      supervisor_forced = true;
      owned_collected = true;
      if (force_deadline >= 0LL) {
        force_deadline += CT_SUPERVISOR_FORCE_MILLISECONDS;
        if (ct_supervisor_process_collect_descendants(
                supervisor, &owned, force_deadline, &process_options) != 0) {
          cleanup_failed = true;
        }
      }
      if (ct_supervisor_process_signal_anchor(
              supervisor, group_established, SIGKILL, force_deadline,
              &process_options) != 0 &&
          errno != ESRCH) {
        cleanup_failed = true;
      }
      force_deadline = ct_supervisor_now();
      if (force_deadline < 0LL) {
        cleanup_failed = true;
      } else {
        force_deadline += CT_SUPERVISOR_FORCE_MILLISECONDS;
        if (ct_supervisor_wait(supervisor, &status, &child_done,
                               force_deadline) != 0) cleanup_failed = true;
      }
    }
  }
  if (supervisor_forced && child_done && owned_collected) {
    long long adopted_deadline = ct_supervisor_now();
    if (adopted_deadline < 0LL) {
      cleanup_failed = true;
    } else {
      adopted_deadline += CT_SUPERVISOR_FORCE_MILLISECONDS;
      if (ct_supervisor_process_cleanup_adopted(&owned, adopted_deadline) != 0) {
        cleanup_failed = true;
      }
    }
  }
  if (child_done) {
    const ssize_t count = read(report[0], &result, 1U);
    if (count != 1 || result != 'S') cleanup_failed = true;
  } else if (supervisor > 0) {
    cleanup_failed = true;
  }

finish:
  if (start_gate[0] >= 0 && close(start_gate[0]) != 0) cleanup_failed = true;
  if (start_gate[1] >= 0 && close(start_gate[1]) != 0) cleanup_failed = true;
  if (ready[0] >= 0 && close(ready[0]) != 0) cleanup_failed = true;
  if (ready[1] >= 0 && close(ready[1]) != 0) cleanup_failed = true;
  if (report[0] >= 0 && close(report[0]) != 0) cleanup_failed = true;
  if (report[1] >= 0 && close(report[1]) != 0) cleanup_failed = true;
  if (ct_supervisor_process_owned_close(&owned) != 0) cleanup_failed = true;
  if (ct_supervisor_restore(&blocked, &old_mask, interrupt_installed,
                            terminate_installed, child_normalized,
                            &old_interrupt, &old_terminate, &old_child) != 0) {
    cleanup_failed = true;
  }
  if (ct_supervisor_interrupted != 0) {
    const int interrupted = (int)ct_supervisor_interrupted;
    ct_supervisor_interrupted = 0;
    if (raise(interrupted) != 0) return 125;
    return 128 + interrupted;
  }
  if (cleanup_failed || !child_done) return 125;
  return timed_out ? 124 : ct_supervisor_status(status);
}
