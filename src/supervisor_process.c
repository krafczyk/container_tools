/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "supervisor_process.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CT_SUPERVISOR_PROCESS_POLL_MILLISECONDS 10LL
#define CT_SUPERVISOR_PROCESS_CHILDREN_CAPACITY 65536U

struct ct_supervisor_process_pids {
  pid_t values[CT_SUPERVISOR_PROCESS_CAPACITY];
  size_t count;
};

struct ct_supervisor_process_identity {
  pid_t pid;
  unsigned long long start_time;
  int pidfd;
};

long long ct_supervisor_process_now(void)
{
  struct timespec value;
  return clock_gettime(CLOCK_MONOTONIC, &value) == 0
             ? (long long)value.tv_sec * 1000LL + value.tv_nsec / 1000000L
             : -1LL;
}

int ct_supervisor_process_pause(long long deadline)
{
  struct timespec pause;
  long long now = ct_supervisor_process_now();
  long long remaining;
  if (now < 0LL) return 1;
  remaining = deadline - now;
  if (remaining <= 0LL) return 0;
  if (remaining > CT_SUPERVISOR_PROCESS_POLL_MILLISECONDS) {
    remaining = CT_SUPERVISOR_PROCESS_POLL_MILLISECONDS;
  }
  pause.tv_sec = 0;
  pause.tv_nsec = remaining * 1000000L;
  return nanosleep(&pause, NULL) != 0 && errno != EINTR ? 1 : 0;
}

void ct_supervisor_process_owned_init(struct ct_supervisor_process_owned *owned)
{
  owned->count = 0U;
}

static int ct_supervisor_process_pidfd_open(
    pid_t pid, const struct ct_supervisor_process_options *options)
{
#ifdef CT_SUPERVISOR_TEST_SEAM
  if (options->force_pidfd_unavailable) {
    errno = ENOSYS;
    return -1;
  }
#else
  (void)options;
#endif
#ifdef SYS_pidfd_open
  return (int)syscall(SYS_pidfd_open, pid, 0U);
#else
  (void)pid;
  errno = ENOSYS;
  return -1;
#endif
}

static int ct_supervisor_process_pidfd_signal(int pidfd, int signal_number)
{
#ifdef SYS_pidfd_send_signal
  return (int)syscall(SYS_pidfd_send_signal, pidfd, signal_number, NULL, 0U);
#else
  (void)pidfd;
  (void)signal_number;
  errno = ENOSYS;
  return -1;
#endif
}

static int ct_supervisor_process_add_pid(
    struct ct_supervisor_process_pids *result, pid_t value)
{
  for (size_t index = 0U; index < result->count; ++index) {
    if (result->values[index] == value) return 0;
  }
  if (result->count == CT_SUPERVISOR_PROCESS_CAPACITY) return 1;
  result->values[result->count++] = value;
  return 0;
}

static int ct_supervisor_process_read_child_file(
    pid_t parent, pid_t thread, struct ct_supervisor_process_pids *result,
    long long deadline)
{
  char children[CT_SUPERVISOR_PROCESS_CHILDREN_CAPACITY + 1U];
  char path[96];
  size_t length = 0U;
  int descriptor;
  const int path_length = snprintf(path, sizeof(path),
                                   "/proc/%ld/task/%ld/children",
                                   (long)parent, (long)thread);
  if (path_length < 0 || path_length >= (int)sizeof(path)) return 1;
  descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0) return errno == ENOENT ? 2 : 1;
  while (length < CT_SUPERVISOR_PROCESS_CHILDREN_CAPACITY) {
    const ssize_t count = read(
        descriptor, children + length,
        CT_SUPERVISOR_PROCESS_CHILDREN_CAPACITY - length);
    if (count > 0) {
      if ((size_t)count >
          CT_SUPERVISOR_PROCESS_CHILDREN_CAPACITY - length) {
        (void)close(descriptor);
        return 1;
      }
      length += (size_t)count;
      continue;
    }
    if (count == 0) break;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      if (ct_supervisor_process_now() >= deadline ||
          ct_supervisor_process_pause(deadline) != 0) {
        (void)close(descriptor);
        return 1;
      }
      continue;
    }
    (void)close(descriptor);
    return 1;
  }
  if (length == CT_SUPERVISOR_PROCESS_CHILDREN_CAPACITY) {
    char extra;
    const ssize_t count = read(descriptor, &extra, 1U);
    if (count != 0) {
      (void)close(descriptor);
      return 1;
    }
  }
  if (close(descriptor) != 0) return 1;
  if (length > CT_SUPERVISOR_PROCESS_CHILDREN_CAPACITY) return 1;
  children[length] = '\0';
  for (size_t offset = 0U; offset < length;) {
    unsigned long value = 0UL;
    while (offset < length && (children[offset] == ' ' ||
                               children[offset] == '\n' ||
                               children[offset] == '\t')) ++offset;
    if (offset == length) break;
    if (children[offset] < '0' || children[offset] > '9') return 1;
    while (offset < length && children[offset] >= '0' &&
           children[offset] <= '9') {
      const unsigned int digit = (unsigned int)(children[offset] - '0');
      if (value > ((unsigned long)INT_MAX - digit) / 10UL) return 1;
      value = value * 10UL + digit;
      ++offset;
    }
    if (value == 0UL ||
        (offset < length && children[offset] != ' ' &&
         children[offset] != '\n' && children[offset] != '\t')) return 1;
    if (ct_supervisor_process_add_pid(result, (pid_t)value) != 0) return 1;
  }
  return 0;
}

static int ct_supervisor_process_read_children(
    pid_t parent, struct ct_supervisor_process_pids *result,
    long long deadline)
{
  char path[64];
  DIR *tasks;
  struct dirent *entry;
  const int path_length =
      snprintf(path, sizeof(path), "/proc/%ld/task", (long)parent);
  if (path_length < 0 || path_length >= (int)sizeof(path)) return 1;
  tasks = opendir(path);
  if (tasks == NULL) return errno == ENOENT ? 2 : 1;
  result->count = 0U;
  errno = 0;
  while ((entry = readdir(tasks)) != NULL) {
    unsigned long value = 0UL;
    size_t offset = 0U;
    int read_result;
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
    while (entry->d_name[offset] >= '0' && entry->d_name[offset] <= '9') {
      const unsigned int digit =
          (unsigned int)(entry->d_name[offset] - '0');
      if (value > ((unsigned long)INT_MAX - digit) / 10UL) {
        (void)closedir(tasks);
        return 1;
      }
      value = value * 10UL + digit;
      ++offset;
    }
    if (value == 0UL || entry->d_name[offset] != '\0') {
      (void)closedir(tasks);
      return 1;
    }
    read_result = ct_supervisor_process_read_child_file(
        parent, (pid_t)value, result, deadline);
    if (read_result == 2) {
      errno = 0;
      continue;
    }
    if (read_result != 0) {
      (void)closedir(tasks);
      return 1;
    }
    errno = 0;
  }
  if (errno != 0 || closedir(tasks) != 0) return 1;
  return 0;
}

static int ct_supervisor_process_read_identity(
    pid_t pid, unsigned long long *result, char *state, long long deadline)
{
  char contents[4097];
  char path[64];
  size_t length = 0U;
  char *cursor;
  int descriptor;
  const int path_length =
      snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
  if (path_length < 0 || path_length >= (int)sizeof(path)) return 1;
  descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0) return errno == ENOENT ? 2 : 1;
  while (length < sizeof(contents) - 1U) {
    const ssize_t count =
        read(descriptor, contents + length, sizeof(contents) - 1U - length);
    if (count > 0) {
      if ((size_t)count > sizeof(contents) - 1U - length) {
        (void)close(descriptor);
        return 1;
      }
      length += (size_t)count;
      continue;
    }
    if (count == 0) break;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      if (ct_supervisor_process_now() >= deadline ||
          ct_supervisor_process_pause(deadline) != 0) {
        (void)close(descriptor);
        return 1;
      }
      continue;
    }
    (void)close(descriptor);
    return 1;
  }
  if (length == sizeof(contents) - 1U) {
    char extra;
    if (read(descriptor, &extra, 1U) != 0) {
      (void)close(descriptor);
      return 1;
    }
  }
  if (close(descriptor) != 0) return 1;
  if (length >= sizeof(contents)) return 1;
  contents[length] = '\0';
  cursor = strrchr(contents, ')');
  if (cursor == NULL) return 1;
  ++cursor;
  for (unsigned int field = 3U; field <= 22U; ++field) {
    unsigned long long value = 0ULL;
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\0') return 1;
    if (field == 3U && state != NULL) *state = *cursor;
    if (field == 22U) {
      if (*cursor < '0' || *cursor > '9') return 1;
      while (*cursor >= '0' && *cursor <= '9') {
        const unsigned int digit = (unsigned int)(*cursor - '0');
        if (value > (ULLONG_MAX - digit) / 10ULL) return 1;
        value = value * 10ULL + digit;
        ++cursor;
      }
      if (*cursor != ' ' && *cursor != '\n' && *cursor != '\0') return 1;
      *result = value;
      return 0;
    }
    while (*cursor != ' ' && *cursor != '\n' && *cursor != '\0') ++cursor;
  }
  return 1;
}

static int ct_supervisor_process_pin_identity(
    pid_t pid, struct ct_supervisor_process_identity *result,
    long long deadline, const struct ct_supervisor_process_options *options)
{
  char state;
  int identity_result;
  const int pidfd = ct_supervisor_process_pidfd_open(pid, options);
  if (pidfd < 0) return errno == ESRCH ? 2 : 1;
  if (ct_supervisor_process_pidfd_signal(pidfd, 0) != 0 && errno != ESRCH) {
    (void)close(pidfd);
    return 1;
  }
  identity_result = ct_supervisor_process_read_identity(
      pid, &result->start_time, &state, deadline);
  if (identity_result != 0) {
    (void)close(pidfd);
    return identity_result;
  }
  if (state != 'Z' && state != 'X' &&
      ct_supervisor_process_pidfd_signal(pidfd, 0) != 0) {
    const int signal_error = errno;
    (void)close(pidfd);
    return signal_error == ESRCH ? 2 : 1;
  }
  result->pid = pid;
  result->pidfd = pidfd;
  return 0;
}

static int ct_supervisor_process_stop_identity(
    const struct ct_supervisor_process_identity *identity, long long deadline)
{
  unsigned long long start_time;
  char state;
  int identity_result;
  if (ct_supervisor_process_pidfd_signal(identity->pidfd, 0) != 0) {
    return errno == ESRCH ? 2 : 1;
  }
  identity_result = ct_supervisor_process_read_identity(
      identity->pid, &start_time, &state, deadline);
  if (identity_result != 0) return identity_result;
  if (start_time != identity->start_time) return 2;
  if (ct_supervisor_process_pidfd_signal(identity->pidfd, 0) != 0) {
    return errno == ESRCH ? 2 : 1;
  }
  if (ct_supervisor_process_pidfd_signal(identity->pidfd, SIGSTOP) != 0) {
    return errno == ESRCH ? 2 : 1;
  }
  for (;;) {
    identity_result = ct_supervisor_process_read_identity(
        identity->pid, &start_time, &state, deadline);
    if (identity_result != 0) return identity_result;
    if (start_time != identity->start_time) return 2;
    if (state == 'T' || state == 't' || state == 'Z' || state == 'X') return 0;
    if (ct_supervisor_process_now() >= deadline ||
        ct_supervisor_process_pause(deadline) != 0) return 1;
  }
}

static bool ct_supervisor_process_has_owned(
    const struct ct_supervisor_process_owned *owned, pid_t pid,
    size_t *position)
{
  for (size_t index = 0U; index < owned->count; ++index) {
    if (owned->pids[index] == pid) {
      if (position != NULL) *position = index;
      return true;
    }
  }
  return false;
}

int ct_supervisor_process_signal_children(pid_t parent, int signal_number,
                                          long long deadline)
{
  struct ct_supervisor_process_pids children;
  if (ct_supervisor_process_read_children(parent, &children, deadline) != 0) {
    return 1;
  }
  for (size_t index = 0U; index < children.count; ++index) {
    if (kill(children.values[index], signal_number) != 0 && errno != ESRCH) {
      return 1;
    }
  }
  return 0;
}

int ct_supervisor_process_collect_descendants(
    pid_t root, struct ct_supervisor_process_owned *owned, long long deadline,
    const struct ct_supervisor_process_options *options)
{
  struct ct_supervisor_process_pids children;
  struct ct_supervisor_process_identity root_identity;
  bool changed;
  int root_result;
  owned->count = 0U;
  root_result = ct_supervisor_process_pin_identity(root, &root_identity,
                                                   deadline, options);
  if (root_result != 0) return 1;
  root_result =
      ct_supervisor_process_stop_identity(&root_identity, deadline);
  if (root_result != 0) {
    (void)close(root_identity.pidfd);
    return 1;
  }
  do {
    int read_result;
    if (ct_supervisor_process_now() >= deadline) {
      (void)close(root_identity.pidfd);
      return 1;
    }
    read_result =
        ct_supervisor_process_read_children(root, &children, deadline);
    if (read_result != 0) {
      (void)close(root_identity.pidfd);
      return 1;
    }
    changed = false;
    for (size_t index = 0U; index < children.count; ++index) {
      const pid_t child = children.values[index];
      struct ct_supervisor_process_identity identity;
      int identity_result;
      if (ct_supervisor_process_has_owned(owned, child, NULL)) continue;
      if (owned->count == CT_SUPERVISOR_PROCESS_CAPACITY) {
        (void)close(root_identity.pidfd);
        return 1;
      }
      identity_result = ct_supervisor_process_pin_identity(
          child, &identity, deadline, options);
      if (identity_result == 2) continue;
      if (identity_result != 0) {
        (void)close(root_identity.pidfd);
        return 1;
      }
      identity_result =
          ct_supervisor_process_stop_identity(&identity, deadline);
      if (identity_result == 2) {
        (void)close(identity.pidfd);
        continue;
      }
      if (identity_result != 0) {
        (void)close(identity.pidfd);
        (void)close(root_identity.pidfd);
        return 1;
      }
      owned->pids[owned->count] = identity.pid;
      owned->start_times[owned->count] = identity.start_time;
      owned->pidfds[owned->count++] = identity.pidfd;
      changed = true;
#ifdef CT_SUPERVISOR_TEST_SEAM
      if (options->fail_after_first_descendant) {
        (void)close(root_identity.pidfd);
        return 1;
      }
#endif
    }
    for (size_t index = 0U; index < owned->count; ++index) {
      read_result = ct_supervisor_process_read_children(
          owned->pids[index], &children, deadline);
      if (read_result == 2) continue;
      if (read_result != 0) {
        (void)close(root_identity.pidfd);
        return 1;
      }
      for (size_t child_index = 0U; child_index < children.count;
           ++child_index) {
        const pid_t child = children.values[child_index];
        struct ct_supervisor_process_identity identity;
        int identity_result;
        if (ct_supervisor_process_has_owned(owned, child, NULL)) continue;
        if (owned->count == CT_SUPERVISOR_PROCESS_CAPACITY) {
          (void)close(root_identity.pidfd);
          return 1;
        }
        identity_result = ct_supervisor_process_pin_identity(
            child, &identity, deadline, options);
        if (identity_result == 2) continue;
        if (identity_result != 0) {
          (void)close(root_identity.pidfd);
          return 1;
        }
        identity_result =
            ct_supervisor_process_stop_identity(&identity, deadline);
        if (identity_result == 2) {
          (void)close(identity.pidfd);
          continue;
        }
        if (identity_result != 0) {
          (void)close(identity.pidfd);
          (void)close(root_identity.pidfd);
          return 1;
        }
        owned->pids[owned->count] = identity.pid;
        owned->start_times[owned->count] = identity.start_time;
        owned->pidfds[owned->count++] = identity.pidfd;
        changed = true;
      }
    }
  } while (changed);
  return close(root_identity.pidfd) != 0;
}

int ct_supervisor_process_signal_anchor(
    pid_t supervisor, bool group_established, int signal_number,
    long long deadline, const struct ct_supervisor_process_options *options)
{
  struct ct_supervisor_process_identity identity;
  int result;
  if (group_established) return kill(-supervisor, signal_number);
  result = ct_supervisor_process_pin_identity(supervisor, &identity, deadline,
                                              options);
  if (result != 0) return -1;
  result = ct_supervisor_process_pidfd_signal(identity.pidfd, signal_number);
  if (result != 0) {
    const int signal_error = errno;
    (void)close(identity.pidfd);
    errno = signal_error;
    return -1;
  }
  if (close(identity.pidfd) != 0) return -1;
  return result;
}

int ct_supervisor_process_cleanup_adopted(
    const struct ct_supervisor_process_owned *owned, long long deadline)
{
  for (;;) {
    struct ct_supervisor_process_pids children;
    bool owned_remain = false;
    if (ct_supervisor_process_read_children(getpid(), &children, deadline) !=
        0) return 1;
    for (size_t index = 0U; index < children.count; ++index) {
      const pid_t child = children.values[index];
      unsigned long long start_time;
      char state;
      size_t owned_index;
      int identity_result;
      int status;
      pid_t reaped;
      if (!ct_supervisor_process_has_owned(owned, child, &owned_index)) {
        continue;
      }
      if (ct_supervisor_process_pidfd_signal(owned->pidfds[owned_index], 0) !=
          0) {
        if (errno != ESRCH) return 1;
        do {
          reaped = waitpid(child, &status, WNOHANG);
        } while (reaped < 0 && errno == EINTR);
        if (reaped < 0 && errno != ECHILD) return 1;
        continue;
      }
      identity_result = ct_supervisor_process_read_identity(
          child, &start_time, &state, deadline);
      if (identity_result == 2) continue;
      if (identity_result != 0) return 1;
      if (start_time != owned->start_times[owned_index]) continue;
      if (ct_supervisor_process_pidfd_signal(owned->pidfds[owned_index], 0) !=
          0) {
        if (errno == ESRCH) continue;
        return 1;
      }
      owned_remain = true;
      if (state != 'Z' && state != 'X' &&
          ct_supervisor_process_pidfd_signal(owned->pidfds[owned_index],
                                             SIGKILL) != 0 &&
          errno != ESRCH) return 1;
      do {
        reaped = waitpid(child, &status, WNOHANG);
      } while (reaped < 0 && errno == EINTR);
      if (reaped < 0 && errno != ECHILD) return 1;
    }
    if (!owned_remain) return 0;
    if (ct_supervisor_process_now() >= deadline ||
        ct_supervisor_process_pause(deadline) != 0) return 1;
  }
}

int ct_supervisor_process_owned_close(struct ct_supervisor_process_owned *owned)
{
  int failed = 0;
  for (size_t index = 0U; index < owned->count; ++index) {
    if (owned->pidfds[index] >= 0 && close(owned->pidfds[index]) != 0) {
      failed = 1;
    }
    owned->pidfds[index] = -1;
  }
  return failed;
}
