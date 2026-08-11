/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "state.h"

#include "storage.h"
#include "storage_timeout.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static int ct_state_hex(const char *value, size_t length)
{
  size_t index;
  if (value == NULL || strlen(value) != length) return 0;
  for (index = 0U; index < length; ++index) {
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) return 0;
  }
  return 1;
}

static int ct_state_name(const char *name)
{
  return name != NULL && strncmp(name, "mkchad-", 7U) == 0 &&
         ct_state_hex(name + 7U, 32U);
}

static int ct_state_root_valid(const char *root)
{
  return root != NULL && root[0] == '/' && strchr(root, ':') == NULL &&
         strchr(root, ',') == NULL && strchr(root, '\n') == NULL;
}

static int ct_state_write_exact(int descriptor, const char *contents, size_t length)
{
  size_t used = 0U;
  while (used < length) {
    const ssize_t written = ct_storage_timeout_write(descriptor, contents + used,
                                                      length - used);
    if (written <= 0) return 1;
    used += (size_t)written;
  }
  return 0;
}

static int ct_state_read_exact(int descriptor, char *contents, size_t length)
{
  size_t used = 0U;
  while (used < length) {
    const ssize_t received = ct_storage_timeout_read(descriptor, contents + used,
                                                      length - used);
    if (received <= 0) return 1;
    used += (size_t)received;
  }
  return 0;
}

static int ct_state_deadline(struct timespec *deadline)
{
  const char *value = getenv("CT_INSTANCE_LOCK_TIMEOUT");
  char *end;
  double seconds;
  if (value == NULL || value[0] == '\0') value = "10";
  errno = 0;
  seconds = strtod(value, &end);
  if (errno != 0 || end == value || *end != '\0' || seconds <= 0.0 ||
      seconds > 3600.0 || clock_gettime(CLOCK_MONOTONIC, deadline) != 0) return 1;
  deadline->tv_sec += (time_t)seconds;
  deadline->tv_nsec += (long)((seconds - (double)(time_t)seconds) * 1000000000.0);
  if (deadline->tv_nsec >= 1000000000L) {
    ++deadline->tv_sec;
    deadline->tv_nsec -= 1000000000L;
  }
  return 0;
}

int ct_state_pending_path(const char *root, const char *name,
                          char path[CT_STATE_PATH_MAX])
{
  if (!ct_state_root_valid(root) || !ct_state_name(name) || path == NULL ||
      snprintf(path, CT_STATE_PATH_MAX, "%s/%s.pending", root, name) >=
          (int)CT_STATE_PATH_MAX) return 1;
  return 0;
}

int ct_state_prepare_root(const char *root)
{
  struct stat status;

  if (!ct_state_root_valid(root)) return 1;
  if (ct_storage_timeout_lstat(root, &status) == 0) {
    return S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode) &&
                   status.st_uid == geteuid() &&
                   (status.st_mode & 0777U) == 0700U
               ? 0
               : 1;
  }
  return errno == ENOENT ? ct_storage_ensure_private_directory(root) : 1;
}

int ct_state_pending_write(const char *path, const char *name, const char *profile,
                           const char *nonce)
{
  char temporary[CT_STATE_PATH_MAX];
  int descriptor;
  char contents[142];
  int length;
  if (path == NULL || !ct_state_name(name) || !ct_state_hex(profile, 64U) ||
      !ct_state_hex(nonce, 32U) || strlen(path) + 16U >= sizeof(temporary) ||
      snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >=
          (int)sizeof(temporary)) return 1;
  length = snprintf(contents, sizeof(contents), "%s\n%s\n%s\n", name, profile,
                    nonce);
  if (length < 0 || (size_t)length >= sizeof(contents)) return 1;
  descriptor = mkstemp(temporary);
  if (descriptor < 0 || ct_storage_timeout_chmod(temporary, 0600) != 0) {
    if (descriptor >= 0) (void)ct_storage_timeout_close(descriptor);
    (void)ct_storage_timeout_unlink(temporary);
    return 1;
  }
  if (ct_state_write_exact(descriptor, contents, (size_t)length) != 0 ||
      ct_storage_timeout_fsync(descriptor) != 0 ||
      ct_storage_timeout_close(descriptor) != 0) {
    (void)ct_storage_timeout_close(descriptor);
    (void)ct_storage_timeout_unlink(temporary);
    return 1;
  }
  if (ct_storage_timeout_rename(temporary, path) != 0) {
    (void)ct_storage_timeout_unlink(temporary);
    return 1;
  }
  return 0;
}

int ct_state_pending_read(const char *path, const char *name, const char *profile,
                          struct ct_state_pending *record)
{
  struct stat status;
  int descriptor = -1;
  char contents[142];
  char *first, *second, *third, *end;
  if (path == NULL || !ct_state_name(name) || !ct_state_hex(profile, 64U) ||
       record == NULL || (descriptor = ct_storage_timeout_open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0)) < 0 ||
       ct_storage_timeout_fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
       S_ISLNK(status.st_mode) || status.st_uid != geteuid() ||
       (status.st_mode & 0777U) != 0600U || status.st_size <= 0 ||
       status.st_size >= (off_t)sizeof(contents)) {
    if (descriptor >= 0) (void)ct_storage_timeout_close(descriptor);
    return 1;
  }
  if (ct_state_read_exact(descriptor, contents, (size_t)status.st_size) != 0 ||
      ct_storage_timeout_close(descriptor) != 0) return 1;
  contents[status.st_size] = '\0';
  first = contents;
  second = strchr(first, '\n');
  if (second == NULL) return 1;
  *second++ = '\0';
  third = strchr(second, '\n');
  if (third == NULL) return 1;
  *third++ = '\0';
  end = strchr(third, '\n');
  if (end == NULL || end[1] != '\0') return 1;
  *end = '\0';
  if (snprintf(record->name, sizeof(record->name), "%s", first) >= (int)sizeof(record->name) ||
      snprintf(record->profile, sizeof(record->profile), "%s", second) >= (int)sizeof(record->profile) ||
      snprintf(record->nonce, sizeof(record->nonce), "%s", third) >= (int)sizeof(record->nonce)) return 1;
  return strcmp(record->name, name) == 0 && strcmp(record->profile, profile) == 0 &&
         ct_state_hex(record->nonce, 32U) ? 0 : 1;
}

int ct_state_pending_clear(const char *path)
{
  if (path == NULL) return 1;
  if (ct_storage_timeout_unlink(path) == 0 || errno == ENOENT) return 0;
  return 1;
}

int ct_state_identity_write(const char *root, const char *name, const char *image,
                            const char *image_identity, const char *profile)
{
  char path[CT_STATE_PATH_MAX], temporary[CT_STATE_PATH_MAX], contents[5120];
  int descriptor = -1;
  int length;

  if (!ct_state_root_valid(root) || !ct_state_name(name) || image == NULL ||
      strchr(image, '\n') != NULL || image_identity == NULL ||
      strchr(image_identity, '\n') != NULL || !ct_state_hex(profile, 64U) ||
      snprintf(path, sizeof(path), "%s/%s.identity", root, name) >= (int)sizeof(path) ||
      snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >=
          (int)sizeof(temporary)) return 1;
  length = snprintf(contents, sizeof(contents),
                    "name=%s\nimage=%s\nidentity=%s\nprofile=%s\n", name, image,
                    image_identity, profile);
  if (length < 0 || (size_t)length >= sizeof(contents)) return 1;
  descriptor = mkstemp(temporary);
  if (descriptor < 0 || ct_storage_timeout_chmod(temporary, 0600) != 0 ||
      ct_state_write_exact(descriptor, contents, (size_t)length) != 0 ||
      ct_storage_timeout_fsync(descriptor) != 0 ||
      ct_storage_timeout_close(descriptor) != 0) {
    if (descriptor >= 0) (void)ct_storage_timeout_close(descriptor);
    (void)ct_storage_timeout_unlink(temporary);
    return 1;
  }
  if (ct_storage_timeout_rename(temporary, path) != 0) {
    (void)ct_storage_timeout_unlink(temporary);
    return 1;
  }
  return 0;
}

enum ct_state_lock_status ct_state_lock(const char *root, const char *name,
                                        int *descriptor)
{
  char path[CT_STATE_PATH_MAX];
  struct timespec deadline;
  int lock_error = 0;
  if (descriptor == NULL || ct_state_prepare_root(root) != 0 || !ct_state_name(name) ||
       snprintf(path, sizeof(path), "%s/%s.lock", root, name) >= (int)sizeof(path) ||
       ct_state_deadline(&deadline) != 0) return CT_STATE_LOCK_SETUP;
  *descriptor = ct_storage_timeout_open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
  if (*descriptor < 0) return CT_STATE_LOCK_SETUP;
  for (;;) {
    struct timespec now = {0, 0}, pause = {0, 10000000L};
    if (flock(*descriptor, LOCK_EX | LOCK_NB) == 0) return CT_STATE_LOCK_OK;
    lock_error = errno;
    if (lock_error != EWOULDBLOCK && lock_error != EAGAIN) break;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      lock_error = 0;
      break;
    }
    if (now.tv_sec > deadline.tv_sec ||
        (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) break;
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) { }
  }
  (void)ct_storage_timeout_close(*descriptor);
  *descriptor = -1;
  return lock_error == EWOULDBLOCK || lock_error == EAGAIN ?
             CT_STATE_LOCK_TIMEOUT : CT_STATE_LOCK_SETUP;
}

void ct_state_unlock(int descriptor)
{
  if (descriptor >= 0) {
    (void)ct_storage_timeout_flock_unlock(descriptor);
    (void)ct_storage_timeout_close(descriptor);
  }
}
