/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_STORAGE_TIMEOUT_H
#define CONTAINER_TOOLS_STORAGE_TIMEOUT_H

#include <dirent.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

/** Return whether CT_RUNTIME_STORAGE_TIMEOUT is an accepted positive decimal deadline. */
int ct_storage_timeout_is_valid(void);

/** Run one filesystem metadata or mutation operation under the storage deadline. */
int ct_storage_timeout_lstat(const char *path, struct stat *status);
int ct_storage_timeout_stat(const char *path, struct stat *status);
int ct_storage_timeout_open(const char *path, int flags, mode_t mode);
int ct_storage_timeout_fstat(int descriptor, struct stat *status);
ssize_t ct_storage_timeout_read(int descriptor, void *buffer, size_t count);
int ct_storage_timeout_close(int descriptor);
int ct_storage_timeout_fsync(int descriptor);
ssize_t ct_storage_timeout_write(int descriptor, const void *buffer, size_t count);
int ct_storage_timeout_flock_lock(int descriptor);
/**
 * Acquire a shared advisory lock under the configured storage deadline.
 *
 * @param descriptor Open descriptor whose advisory lock is acquired.
 * @return Zero on success, or -1 with errno set on timeout or lock failure.
 * @sideeffect Holds a shared flock until explicitly unlocked or closed.
 */
int ct_storage_timeout_flock_shared(int descriptor);
int ct_storage_timeout_flock_unlock(int descriptor);
int ct_storage_timeout_mkdir(const char *path, mode_t mode);
int ct_storage_timeout_chmod(const char *path, mode_t mode);
int ct_storage_timeout_access(const char *path, int mode);
DIR *ct_storage_timeout_opendir(const char *path);
struct dirent *ct_storage_timeout_readdir(DIR *directory);
int ct_storage_timeout_closedir(DIR *directory);
int ct_storage_timeout_unlink(const char *path);
int ct_storage_timeout_rmdir(const char *path);
int ct_storage_timeout_rename(const char *old_path, const char *new_path);
int ct_storage_timeout_link(const char *old_path, const char *new_path);

#endif
