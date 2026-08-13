/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "storage_timeout.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define CT_STORAGE_TIMEOUT_MAX_SECONDS 3600U

struct ct_storage_timeout {
  struct sigaction previous_action;
  struct itimerval previous_timer;
  sigset_t previous_mask;
  bool active;
};

static volatile sig_atomic_t ct_storage_timeout_expired;

static void ct_storage_timeout_signal(int signal_number)
{
  (void)signal_number;
  ct_storage_timeout_expired = 1;
}

static int ct_storage_timeout_value(struct itimerval *timer)
{
  const char *value = getenv("CT_RUNTIME_STORAGE_TIMEOUT");
  unsigned long seconds = 0U;
  unsigned long microseconds = 0U;
  size_t index = 0U;
  size_t fractional_digits = 0U;
  bool nonzero = false;
  bool round_up = false;

  if (value == NULL) value = "10";
  if (value[0] == '\0') return 1;
  while (value[index] >= '0' && value[index] <= '9') {
    const unsigned long digit = (unsigned long)(value[index] - '0');
    if (seconds > CT_STORAGE_TIMEOUT_MAX_SECONDS / 10U ||
        (seconds == CT_STORAGE_TIMEOUT_MAX_SECONDS / 10U &&
         digit > CT_STORAGE_TIMEOUT_MAX_SECONDS % 10U)) return 1;
    seconds = seconds * 10U + digit;
    nonzero = nonzero || digit != 0U;
    ++index;
  }
  if (index == 0U) return 1;
  if (value[index] == '.') {
    ++index;
    while (value[index] >= '0' && value[index] <= '9') {
      const unsigned long digit = (unsigned long)(value[index] - '0');
      nonzero = nonzero || digit != 0U;
      if (fractional_digits < 6U) {
        microseconds = microseconds * 10U + digit;
      } else if (digit != 0U) {
        round_up = true;
      }
      ++fractional_digits;
      ++index;
    }
    if (fractional_digits == 0U) return 1;
  }
  if (value[index] != '\0' || !nonzero || seconds > CT_STORAGE_TIMEOUT_MAX_SECONDS) return 1;
  while (fractional_digits < 6U) {
    microseconds *= 10U;
    ++fractional_digits;
  }
  if (round_up) {
    ++microseconds;
    if (microseconds == 1000000U) {
      microseconds = 0U;
      ++seconds;
    }
  }
  if (seconds > CT_STORAGE_TIMEOUT_MAX_SECONDS ||
      (seconds == CT_STORAGE_TIMEOUT_MAX_SECONDS && microseconds != 0U)) return 1;
  timer->it_interval.tv_sec = 0;
  timer->it_interval.tv_usec = 0;
  timer->it_value.tv_sec = (time_t)seconds;
  timer->it_value.tv_usec = (suseconds_t)microseconds;
  return 0;
}

int ct_storage_timeout_is_valid(void)
{
  struct itimerval timer;
  return ct_storage_timeout_value(&timer) == 0;
}

static int ct_storage_timeout_end(struct ct_storage_timeout *timeout);

static int ct_storage_timeout_begin(struct ct_storage_timeout *timeout)
{
  struct itimerval timer;
  struct sigaction action;
  sigset_t blocked;
  sigset_t pending;

  if (timeout == NULL || ct_storage_timeout_value(&timer) != 0) {
    errno = EINVAL;
    return 1;
  }
#ifdef CT_STORAGE_TIMEOUT_TEST_SEAM
  if (getenv("CT_TEST_STORAGE_TIMEOUT_FORCE") != NULL) {
    errno = ETIMEDOUT;
    return 1;
  }
#endif
  (void)memset(timeout, 0, sizeof(*timeout));
  if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGALRM) != 0 ||
      sigprocmask(SIG_BLOCK, &blocked, &timeout->previous_mask) != 0) return 1;
  if (sigpending(&pending) != 0 || sigismember(&pending, SIGALRM) == 1 ||
      sigaction(SIGALRM, NULL, &timeout->previous_action) != 0 ||
      getitimer(ITIMER_REAL, &timeout->previous_timer) != 0 ||
      timeout->previous_timer.it_value.tv_sec != 0 ||
      timeout->previous_timer.it_value.tv_usec != 0) {
    (void)sigprocmask(SIG_SETMASK, &timeout->previous_mask, NULL);
    errno = EBUSY;
    return 1;
  }
  action.sa_handler = ct_storage_timeout_signal;
  action.sa_flags = 0;
  if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGALRM, &action, NULL) != 0 ||
      setitimer(ITIMER_REAL, &timer, NULL) != 0) {
    (void)sigaction(SIGALRM, &timeout->previous_action, NULL);
    (void)setitimer(ITIMER_REAL, &timeout->previous_timer, NULL);
    (void)sigprocmask(SIG_SETMASK, &timeout->previous_mask, NULL);
    return 1;
  }
  ct_storage_timeout_expired = 0;
  timeout->active = true;
  if (sigprocmask(SIG_SETMASK, &timeout->previous_mask, NULL) != 0) {
    (void)ct_storage_timeout_end(timeout);
    return 1;
  }
  return 0;
}

static int ct_storage_timeout_end(struct ct_storage_timeout *timeout)
{
  sigset_t blocked;
  struct timespec immediate = {0, 0};
  int timed_out;

  int result = 0;

  if (timeout == NULL || !timeout->active) return 0;
  if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGALRM) != 0 ||
      sigprocmask(SIG_BLOCK, &blocked, NULL) != 0) {
    result = 1;
  }
  timed_out = ct_storage_timeout_expired != 0;
  if (result == 0 && sigtimedwait(&blocked, NULL, &immediate) == SIGALRM) timed_out = 1;
  if (setitimer(ITIMER_REAL, &timeout->previous_timer, NULL) != 0) result = 1;
  if (sigaction(SIGALRM, &timeout->previous_action, NULL) != 0) result = 1;
  if (sigprocmask(SIG_SETMASK, &timeout->previous_mask, NULL) != 0) result = 1;
  timeout->active = false;
  if (result != 0 || timed_out != 0) {
    errno = ETIMEDOUT;
    return 1;
  }
  return 0;
}

int ct_storage_timeout_call(int (*operation)(void *), void *context)
{
  struct ct_storage_timeout timeout;
  int result;
  int operation_errno;
  if (operation == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (ct_storage_timeout_begin(&timeout) != 0) return -1;
  result = operation(context);
  operation_errno = errno;
  if (ct_storage_timeout_end(&timeout) != 0) return -1;
  errno = operation_errno;
  return result;
}

#define CT_STORAGE_TIMEOUT_INT_CALL(call) \
  do { \
    struct ct_storage_timeout timeout; \
    int result; \
    int operation_errno; \
    if (ct_storage_timeout_begin(&timeout) != 0) return -1; \
    result = (call); \
    operation_errno = errno; \
    if (ct_storage_timeout_end(&timeout) != 0) return -1; \
    errno = operation_errno; \
    return result; \
  } while (0)

static void ct_storage_timeout_present_stat(struct stat *status)
{
#ifdef CT_STORAGE_TIMEOUT_TEST_SEAM
  if (status != NULL && getenv("CT_TEST_STORAGE_PRESENT_FOREIGN_UID") != NULL) {
    status->st_uid = geteuid() == 0 ? 1 : 0;
  }
#else
  (void)status;
#endif
}

int ct_storage_timeout_lstat(const char *path, struct stat *status)
{
  CT_STORAGE_TIMEOUT_INT_CALL(
      lstat(path, status) == 0 ? (ct_storage_timeout_present_stat(status), 0) : -1);
}

int ct_storage_timeout_stat(const char *path, struct stat *status)
{
  CT_STORAGE_TIMEOUT_INT_CALL(
      stat(path, status) == 0 ? (ct_storage_timeout_present_stat(status), 0) : -1);
}

int ct_storage_timeout_open(const char *path, int flags, mode_t mode)
{
  CT_STORAGE_TIMEOUT_INT_CALL(open(path, flags, mode));
}

int ct_storage_timeout_fstat(int descriptor, struct stat *status)
{
  CT_STORAGE_TIMEOUT_INT_CALL(
      fstat(descriptor, status) == 0 ? (ct_storage_timeout_present_stat(status), 0) : -1);
}

ssize_t ct_storage_timeout_read(int descriptor, void *buffer, size_t count)
{
  struct ct_storage_timeout timeout;
  ssize_t result;
  int operation_errno;
  if (ct_storage_timeout_begin(&timeout) != 0) return -1;
  result = read(descriptor, buffer, count);
  operation_errno = errno;
  if (ct_storage_timeout_end(&timeout) != 0) return -1;
  errno = operation_errno;
  return result;
}

int ct_storage_timeout_close(int descriptor)
{
  CT_STORAGE_TIMEOUT_INT_CALL(close(descriptor));
}

int ct_storage_timeout_fsync(int descriptor)
{
  CT_STORAGE_TIMEOUT_INT_CALL(fsync(descriptor));
}

ssize_t ct_storage_timeout_write(int descriptor, const void *buffer, size_t count)
{
  struct ct_storage_timeout timeout;
  ssize_t result;
  int operation_errno;
  if (ct_storage_timeout_begin(&timeout) != 0) return -1;
  result = write(descriptor, buffer, count);
  operation_errno = errno;
  if (ct_storage_timeout_end(&timeout) != 0) return -1;
  errno = operation_errno;
  return result;
}

int ct_storage_timeout_flock_lock(int descriptor)
{
  CT_STORAGE_TIMEOUT_INT_CALL(flock(descriptor, LOCK_EX));
}

int ct_storage_timeout_flock_shared(int descriptor)
{
  CT_STORAGE_TIMEOUT_INT_CALL(flock(descriptor, LOCK_SH));
}

int ct_storage_timeout_flock_unlock(int descriptor)
{
  CT_STORAGE_TIMEOUT_INT_CALL(flock(descriptor, LOCK_UN));
}

int ct_storage_timeout_mkdir(const char *path, mode_t mode)
{
  CT_STORAGE_TIMEOUT_INT_CALL(mkdir(path, mode));
}

int ct_storage_timeout_access(const char *path, int mode)
{
  CT_STORAGE_TIMEOUT_INT_CALL(access(path, mode));
}

DIR *ct_storage_timeout_opendir(const char *path)
{
  struct ct_storage_timeout timeout;
  DIR *result;
  int operation_errno;
#ifdef CT_STORAGE_TIMEOUT_TEST_SEAM
  {
    const char *forced_path = getenv("CT_TEST_STORAGE_OPENDIR_FAIL");
    if (forced_path != NULL && strcmp(forced_path, path) == 0) {
      errno = getenv("CT_TEST_STORAGE_OPENDIR_TIMEOUT") == NULL ? EACCES : ETIMEDOUT;
      return NULL;
    }
  }
#endif
  if (ct_storage_timeout_begin(&timeout) != 0) return NULL;
  result = opendir(path);
  operation_errno = errno;
  if (ct_storage_timeout_end(&timeout) != 0) return NULL;
  errno = operation_errno;
  return result;
}

struct dirent *ct_storage_timeout_readdir(DIR *directory)
{
  struct ct_storage_timeout timeout;
  struct dirent *result;
  int operation_errno;
  if (ct_storage_timeout_begin(&timeout) != 0) return NULL;
  errno = 0;
  result = readdir(directory);
  operation_errno = errno;
  if (ct_storage_timeout_end(&timeout) != 0) return NULL;
  errno = operation_errno;
  return result;
}

int ct_storage_timeout_closedir(DIR *directory)
{
  CT_STORAGE_TIMEOUT_INT_CALL(closedir(directory));
}

int ct_storage_timeout_unlink(const char *path)
{
  CT_STORAGE_TIMEOUT_INT_CALL(unlink(path));
}

int ct_storage_timeout_rmdir(const char *path)
{
  CT_STORAGE_TIMEOUT_INT_CALL(rmdir(path));
}

int ct_storage_timeout_rename(const char *old_path, const char *new_path)
{
  CT_STORAGE_TIMEOUT_INT_CALL(rename(old_path, new_path));
}

int ct_storage_timeout_link(const char *old_path, const char *new_path)
{
  CT_STORAGE_TIMEOUT_INT_CALL(link(old_path, new_path));
}
