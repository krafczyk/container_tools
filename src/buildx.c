/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "buildx.h"

#include "process.h"
#include "storage.h"
#include "storage_timeout.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CT_BUILDX_REMOVE_MAX_DEPTH 64U
#define CT_BUILDX_REMOVE_MAX_ENTRIES 65536U
#define CT_BUILDX_NAMESPACE_MAX_ENTRIES 4096U

static int ct_buildx_remove_tree_at(const char *path, size_t depth,
                                    size_t *entry_count)
{
  DIR *directory;
  struct dirent *entry;
  struct stat status;

  if (entry_count == NULL || depth > CT_BUILDX_REMOVE_MAX_DEPTH ||
      *entry_count >= CT_BUILDX_REMOVE_MAX_ENTRIES) return 1;
  ++*entry_count;
  if (ct_storage_timeout_lstat(path, &status) != 0) return errno == ENOENT ? 0 : 1;
  if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) return ct_storage_timeout_unlink(path) == 0 ? 0 : 1;
  directory = ct_storage_timeout_opendir(path);
  if (directory == NULL) return errno == ENOENT ? 0 : 1;
  errno = 0;
  while ((entry = ct_storage_timeout_readdir(directory)) != NULL) {
    char child[CT_BUILDX_PATH_MAX];
    struct stat child_status;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
        ct_storage_timeout_lstat(child, &child_status) != 0) { (void)ct_storage_timeout_closedir(directory); return 1; }
    if (S_ISDIR(child_status.st_mode) && !S_ISLNK(child_status.st_mode)) {
      if (ct_buildx_remove_tree_at(child, depth + 1U, entry_count) != 0) {
        (void)ct_storage_timeout_closedir(directory);
        return 1;
      }
    } else if (ct_storage_timeout_unlink(child) != 0) { (void)ct_storage_timeout_closedir(directory); return 1; }
  }
  if (errno != 0 || ct_storage_timeout_closedir(directory) != 0) return 1;
  return ct_storage_timeout_rmdir(path) == 0 ? 0 : 1;
}

static int ct_buildx_remove_tree(const char *path)
{
  size_t entry_count = 0U;
  return ct_buildx_remove_tree_at(path, 0U, &entry_count);
}

static int ct_buildx_exists(const char *path)
{
  struct stat status;
  if (ct_storage_timeout_lstat(path, &status) == 0) return 1;
  return errno == ENOENT ? 0 : -1;
}

static int ct_buildx_timeout_is_valid(void)
{
  const char *value = getenv("CT_DOCKER_BUILD_LOCK_TIMEOUT");
  char *end;
  double seconds;

  if (value == NULL) return 1;
  errno = 0;
  seconds = strtod(value, &end);
  return value[0] != '\0' && end != value && *end == '\0' && errno != ERANGE &&
         isfinite(seconds) && seconds > 0.0 && seconds <= 3600.0;
}

static int ct_buildx_lock(int descriptor)
{
  const char *value = getenv("CT_DOCKER_BUILD_LOCK_TIMEOUT");
  const double seconds = value == NULL ? 30.0 : strtod(value, NULL);
  struct timespec pause = {0, 10000000L};
  double waited = 0.0;

  if (value != NULL && !ct_buildx_timeout_is_valid()) return 1;
  while (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    if (errno != EWOULDBLOCK && errno != EAGAIN) return 1;
    if (waited >= seconds) return 1;
    (void)nanosleep(&pause, NULL);
    waited += 0.01;
  }
  return 0;
}

static int ct_buildx_recover(struct ct_buildx_transaction *transaction)
{
  char previous[CT_BUILDX_PATH_MAX];
  DIR *directory;
  struct dirent *entry;
  int exists;
  size_t entry_count = 0U;

  if (snprintf(previous, sizeof(previous), "%s/.previous", transaction->namespace_path) >=
      (int)sizeof(previous)) return 1;
  exists = ct_buildx_exists(previous);
  if (exists < 0) return 1;
  if (exists > 0) {
    const int current_exists = ct_buildx_exists(transaction->current_path);
    if (current_exists < 0 || (current_exists > 0 && ct_buildx_remove_tree(previous) != 0) ||
        (current_exists == 0 && ct_storage_timeout_rename(previous, transaction->current_path) != 0)) return 1;
  }
  directory = ct_storage_timeout_opendir(transaction->namespace_path);
  if (directory == NULL) return 1;
  errno = 0;
  while ((entry = ct_storage_timeout_readdir(directory)) != NULL) {
    char stale[CT_BUILDX_PATH_MAX];
    if (++entry_count > CT_BUILDX_NAMESPACE_MAX_ENTRIES) {
      (void)ct_storage_timeout_closedir(directory);
      return 1;
    }
    if (strncmp(entry->d_name, ".next.", 6U) != 0) continue;
    if (snprintf(stale, sizeof(stale), "%s/%s", transaction->namespace_path, entry->d_name) >=
        (int)sizeof(stale) || ct_buildx_remove_tree(stale) != 0) {
      (void)ct_storage_timeout_closedir(directory);
      return 1;
    }
  }
  return errno == 0 && ct_storage_timeout_closedir(directory) == 0 ? 0 : 1;
}

bool ct_buildx_child_is_valid(const char *const *arguments, size_t argument_count)
{
  size_t index;
  if (arguments == NULL || argument_count < 3U || strcmp(arguments[0], "docker") != 0 ||
      strcmp(arguments[1], "buildx") != 0 || strcmp(arguments[2], "build") != 0) return false;
  for (index = 3U; index < argument_count; ++index) {
    if (strcmp(arguments[index], "--cache-from") == 0 || strcmp(arguments[index], "--cache-to") == 0 ||
        strncmp(arguments[index], "--cache-from=", 13U) == 0 ||
        strncmp(arguments[index], "--cache-to=", 11U) == 0) return false;
  }
  return true;
}

int ct_buildx_prepare(struct ct_buildx_transaction *transaction,
                      const struct ct_runtime_config *config,
                      const char *architecture)
{
  char lock_path[CT_BUILDX_PATH_MAX];
  char index_path[CT_BUILDX_PATH_MAX];
  const char *cache_root;
  int current_exists;

  if (transaction == NULL || config == NULL || !ct_storage_architecture_is_safe(architecture) ||
      !ct_storage_timeout_is_valid() ||
      (getenv("CT_DOCKER_BUILD_LOCK_TIMEOUT") != NULL && !ct_buildx_timeout_is_valid())) return 1;
  memset(transaction, 0, sizeof(*transaction));
  transaction->lock_descriptor = -1;
  cache_root = getenv("CT_DOCKER_BUILD_CACHE_DIR");
  if (cache_root == NULL || cache_root[0] == '\0') cache_root = config->docker_build_cache_dir;
  if (cache_root[0] == '\0') return 0;
  if (strchr(cache_root, ',') != NULL || ct_storage_ensure_private_directory(cache_root) != 0 ||
      snprintf(transaction->namespace_path, sizeof(transaction->namespace_path), "%s/%s",
               cache_root, architecture) >= (int)sizeof(transaction->namespace_path) ||
      ct_storage_ensure_private_directory(transaction->namespace_path) != 0 ||
      snprintf(transaction->current_path, sizeof(transaction->current_path), "%s/current",
               transaction->namespace_path) >= (int)sizeof(transaction->current_path) ||
      snprintf(lock_path, sizeof(lock_path), "%s/.lock", transaction->namespace_path) >= (int)sizeof(lock_path)) {
    return 1;
  }
  transaction->lock_descriptor = ct_storage_timeout_open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (transaction->lock_descriptor < 0 || ct_buildx_lock(transaction->lock_descriptor) != 0 ||
      ct_buildx_recover(transaction) != 0) {
    (void)ct_buildx_discard(transaction);
    return 1;
  }
  current_exists = ct_buildx_exists(transaction->current_path);
  if (current_exists < 0) {
    (void)ct_buildx_discard(transaction);
    return 1;
  }
  transaction->has_current = current_exists > 0;
  if (transaction->has_current) {
    if (ct_storage_ensure_private_directory(transaction->current_path) != 0 ||
        snprintf(index_path, sizeof(index_path), "%s/index.json", transaction->current_path) >=
            (int)sizeof(index_path)) { (void)ct_buildx_discard(transaction); return 1; }
    if (ct_storage_timeout_access(index_path, F_OK) != 0) {
      if (errno != ENOENT) {
        (void)ct_buildx_discard(transaction);
        return 1;
      }
      transaction->has_current = false;
    }
  }
  if (snprintf(transaction->staging_path, sizeof(transaction->staging_path), "%s/.next.%ld",
               transaction->namespace_path, (long)getpid()) >= (int)sizeof(transaction->staging_path) ||
      ct_storage_ensure_private_directory(transaction->staging_path) != 0) {
    (void)ct_buildx_discard(transaction);
    return 1;
  }
  transaction->enabled = true;
  return 0;
}

int ct_buildx_commit(struct ct_buildx_transaction *transaction)
{
  char previous[CT_BUILDX_PATH_MAX];
  char index_path[CT_BUILDX_PATH_MAX];
  int current_exists;

  if (transaction == NULL || !transaction->enabled) return ct_buildx_discard(transaction);
  if (snprintf(index_path, sizeof(index_path), "%s/index.json", transaction->staging_path) >=
          (int)sizeof(index_path) || ct_storage_timeout_access(index_path, F_OK) != 0 ||
      snprintf(previous, sizeof(previous), "%s/.previous", transaction->namespace_path) >=
          (int)sizeof(previous) || ct_buildx_remove_tree(previous) != 0) return 1;
  current_exists = ct_buildx_exists(transaction->current_path);
  if (current_exists < 0 ||
      (current_exists > 0 && ct_storage_timeout_rename(transaction->current_path, previous) != 0)) return 1;
  if (ct_storage_timeout_rename(transaction->staging_path, transaction->current_path) != 0) {
    const int previous_exists = ct_buildx_exists(previous);
    if (previous_exists > 0) (void)ct_storage_timeout_rename(previous, transaction->current_path);
    return 1;
  }
  transaction->staging_path[0] = '\0';
  if (ct_buildx_remove_tree(previous) != 0) return 1;
  return ct_buildx_discard(transaction);
}

int ct_buildx_discard(struct ct_buildx_transaction *transaction)
{
  int result = 0;
  if (transaction == NULL) return 1;
  if (transaction->staging_path[0] != '\0' && ct_buildx_remove_tree(transaction->staging_path) != 0) result = 1;
  transaction->staging_path[0] = '\0';
  if (transaction->lock_descriptor >= 0) {
    if (ct_storage_timeout_flock_unlock(transaction->lock_descriptor) != 0) result = 1;
    if (ct_storage_timeout_close(transaction->lock_descriptor) != 0) result = 1;
    transaction->lock_descriptor = -1;
  }
  transaction->enabled = false;
  return result;
}

int ct_buildx_exec_command(int argument_count, char *const arguments[])
{
  struct ct_runtime_config config;
  struct ct_buildx_transaction transaction;
  struct ct_process_environment environment[1];
  char *child[4096];
  const char *architecture = NULL;
  int separator = -1;
  int index;
  size_t child_count = 0U;
  int result;
  int transaction_result;
  const char *temporary;

  if (getenv("CT_DOCKER_BUILD_LOCK_TIMEOUT") != NULL &&
      !ct_buildx_timeout_is_valid()) return 1;

  for (index = 0; index < argument_count; ++index) {
    if (strcmp(arguments[index], "--architecture") == 0 && index + 1 < argument_count && architecture == NULL) architecture = arguments[++index];
    else if (strcmp(arguments[index], "--") == 0) { separator = index; break; }
    else return 64;
  }
  if (architecture == NULL || separator < 0 || separator + 3 >= argument_count ||
      !ct_buildx_child_is_valid((const char *const *)(arguments + separator + 1),
                                (size_t)(argument_count - separator - 1)) ||
      ct_runtime_config_load_environment(&config) != CT_CONFIG_OK ||
      ct_storage_select_runtime("docker", &config) != 0 ||
      ct_buildx_prepare(&transaction, &config, architecture) != 0) return 1;
  child[child_count++] = arguments[separator + 1]; child[child_count++] = arguments[separator + 2]; child[child_count++] = arguments[separator + 3];
  if (transaction.enabled && transaction.has_current) {
    static char from[CT_BUILDX_PATH_MAX + 32U];
    (void)snprintf(from, sizeof(from), "type=local,src=%s", transaction.current_path);
    child[child_count++] = "--cache-from"; child[child_count++] = from;
  }
  if (transaction.enabled) {
    static char to[CT_BUILDX_PATH_MAX + 40U];
    (void)snprintf(to, sizeof(to), "type=local,dest=%s,mode=max", transaction.staging_path);
    child[child_count++] = "--cache-to"; child[child_count++] = to;
  }
  for (index = separator + 4; index < argument_count && child_count + 1U < sizeof(child) / sizeof(child[0]); ++index) child[child_count++] = arguments[index];
  if (index < argument_count) {
    (void)ct_buildx_discard(&transaction);
    return 64;
  }
  child[child_count] = NULL;
  temporary = getenv("TMPDIR");
  environment[0].name = "TMPDIR"; environment[0].value = temporary == NULL ? "" : temporary;
  result = ct_process_run(child, environment, environment[0].value[0] == '\0' ? 0U : 1U);
  if (result == 0) {
    transaction_result = ct_buildx_commit(&transaction);
    if (transaction_result != 0) {
      (void)ct_buildx_discard(&transaction);
      result = 1;
    }
  } else {
    (void)ct_buildx_discard(&transaction);
  }
  return result;
}
