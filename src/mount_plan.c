/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "mount_plan.h"

#include "sha256.h"
#include "storage.h"
#include "storage_timeout.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CT_PLAN_FIELD_MAX (7U + CT_MOUNT_PLAN_MAX_ENTRIES * 5U)

static unsigned long ct_plan_temporary_sequence;

static int ct_plan_read_exact(int descriptor, unsigned char *bytes, size_t length);

static bool ct_plan_one_of(const char *value, const char *const values[], size_t count)
{
  size_t index;
  if (value == NULL) return false;
  for (index = 0U; index < count; ++index) if (strcmp(value, values[index]) == 0) return true;
  return false;
}

static bool ct_plan_path(const char *value)
{
  const char *part;
  if (value == NULL || value[0] != '/' || strchr(value, ':') != NULL || strchr(value, ',') != NULL || strchr(value, '\n') != NULL) return false;
  if (strcmp(value, "/") == 0) return true;
  for (part = value + 1; ; ) {
    const char *end = strchr(part, '/');
    const size_t length = end == NULL ? strlen(part) : (size_t)(end - part);
    if (length == 0U || (length == 1U && part[0] == '.') || (length == 2U && part[0] == '.' && part[1] == '.')) return false;
    if (end == NULL) return true;
    part = end + 1;
  }
}

static int ct_plan_entry_valid(const struct ct_mount_plan_entry *entry, const char *backend)
{
  static const char *const roles[] = {"generated-host-root", "detected-automatic", "explicit", "bootstrap-internal", "persistent-automatic-cwd"};
  static const char *const access[] = {"inherit", "read-only"};
  static const char *const recursion[] = {"non-recursive", "runtime-default"};
  if (entry == NULL || !ct_plan_one_of(entry->role, roles, 5U) || !ct_plan_path(entry->caller_path) || !ct_plan_path(entry->target_path) ||
      !ct_plan_one_of(entry->access, access, 2U) || !ct_plan_one_of(entry->recursion, recursion, 2U)) return 1;
  if (strcmp(entry->role, "generated-host-root") == 0) return
    (strcmp(entry->caller_path, "/host") == 0 || strncmp(entry->caller_path, "/host/", 6U) == 0) && strcmp(entry->access, "inherit") == 0 &&
    (((strcmp(backend, "docker") == 0 || strcmp(backend, "podman") == 0) && strcmp(entry->recursion, "non-recursive") == 0) ||
     ((strcmp(backend, "singularity") == 0 || strcmp(backend, "apptainer") == 0) && strcmp(entry->recursion, "runtime-default") == 0)) ? 0 : 1;
  if (strcmp(entry->role, "bootstrap-internal") == 0) return strcmp(entry->caller_path, "/.container-tools-bootstrap") == 0 &&
    strcmp(entry->target_path, "/.container-tools-bootstrap") == 0 && strcmp(entry->access, "read-only") == 0 && strcmp(entry->recursion, "runtime-default") == 0 ? 0 : 1;
  return strcmp(entry->caller_path, entry->target_path) == 0 && strcmp(entry->access, "inherit") == 0 && strcmp(entry->recursion, "runtime-default") == 0 ? 0 : 1;
}

int ct_mount_plan_validate(const struct ct_mount_plan *plan)
{
  static const char *const backends[] = {"docker", "podman", "singularity", "apptainer"};
  static const char *const strategies[] = {"direct", "fallback", "none"};
  static const char *const completeness[] = {"complete", "partial"};
  static const char *const groups[] = {"numeric-supplementary", "keep-groups", "primary-only", "native-inherited", "none"};
  size_t index, bytes;
  if (plan == NULL || !ct_plan_one_of(plan->backend, backends, 4U) || !ct_plan_one_of(plan->strategy, strategies, 3U) ||
      !ct_plan_one_of(plan->completeness, completeness, 2U) || !ct_plan_one_of(plan->group_mode, groups, 5U) || plan->entry_count > CT_MOUNT_PLAN_MAX_ENTRIES ||
      (plan->entry_count != 0U && plan->entries == NULL)) return 1;
  bytes = strlen("ct-mount-plan-v1") + 1U + 65U + strlen(plan->backend) + 1U + strlen(plan->strategy) + 1U + strlen(plan->completeness) + 1U + strlen(plan->group_mode) + 1U + 21U;
  for (index = 0U; index < plan->entry_count; ++index) {
    const struct ct_mount_plan_entry *entry = &plan->entries[index];
    if (ct_plan_entry_valid(entry, plan->backend) != 0) return 1;
    bytes += strlen(entry->role) + strlen(entry->caller_path) + strlen(entry->target_path) + strlen(entry->access) + strlen(entry->recursion) + 5U;
    if (bytes > CT_MOUNT_PLAN_MAX_BYTES) return 1;
  }
  return 0;
}

static int ct_plan_add(unsigned char *output, size_t capacity, size_t *used, const char *value, struct ct_sha256 *hash)
{
  const size_t length = strlen(value) + 1U;
  if (*used > capacity || length > capacity - *used) return 1;
  memcpy(output + *used, value, length);
  if (hash != NULL) ct_sha256_update(hash, output + *used, length);
  *used += length;
  return 0;
}

int ct_mount_plan_serialize(const struct ct_mount_plan *plan, unsigned char **bytes, size_t *length, char digest[65])
{
  unsigned char *result, raw[32];
  struct ct_sha256 hash;
  char count[21];
  size_t capacity = 0U, used = 0U, index;
  if (bytes == NULL || length == NULL || digest == NULL || ct_mount_plan_validate(plan) != 0 ||
      snprintf(count, sizeof(count), "%zu", plan->entry_count) < 0) return 1;
  capacity = strlen("ct-mount-plan-v1") + 1U + 65U + strlen(plan->backend) + 1U + strlen(plan->strategy) + 1U + strlen(plan->completeness) + 1U + strlen(plan->group_mode) + 1U + strlen(count) + 1U;
  for (index = 0U; index < plan->entry_count; ++index) capacity += strlen(plan->entries[index].role) + strlen(plan->entries[index].caller_path) + strlen(plan->entries[index].target_path) + strlen(plan->entries[index].access) + strlen(plan->entries[index].recursion) + 5U;
  result = malloc(capacity);
  if (result == NULL || capacity > CT_MOUNT_PLAN_MAX_BYTES) { free(result); return 1; }
  ct_sha256_init(&hash);
  if (ct_plan_add(result, capacity, &used, "ct-mount-plan-v1", &hash) != 0) { free(result); return 1; }
  /* The digest field is deliberately excluded from the content address. */
  used += 65U;
  if (ct_plan_add(result, capacity, &used, plan->backend, &hash) != 0 || ct_plan_add(result, capacity, &used, plan->strategy, &hash) != 0 ||
      ct_plan_add(result, capacity, &used, plan->completeness, &hash) != 0 || ct_plan_add(result, capacity, &used, plan->group_mode, &hash) != 0 || ct_plan_add(result, capacity, &used, count, &hash) != 0) { free(result); return 1; }
  for (index = 0U; index < plan->entry_count; ++index) {
    const struct ct_mount_plan_entry *entry = &plan->entries[index];
    if (ct_plan_add(result, capacity, &used, entry->role, &hash) != 0 || ct_plan_add(result, capacity, &used, entry->caller_path, &hash) != 0 ||
        ct_plan_add(result, capacity, &used, entry->target_path, &hash) != 0 || ct_plan_add(result, capacity, &used, entry->access, &hash) != 0 || ct_plan_add(result, capacity, &used, entry->recursion, &hash) != 0) { free(result); return 1; }
  }
  ct_sha256_final(&hash, raw); ct_sha256_hex(raw, digest);
  memcpy(result + strlen("ct-mount-plan-v1") + 1U, digest, 64U);
  result[strlen("ct-mount-plan-v1") + 65U] = '\0';
  *bytes = result; *length = used;
  return 0;
}

static int ct_plan_count(const unsigned char *bytes, size_t length, char *fields[CT_PLAN_FIELD_MAX], char *copy, size_t *field_count)
{
  size_t offset = 0U, count = 0U;
  while (offset < length) {
    size_t start = offset;
    if (count == CT_PLAN_FIELD_MAX) return 1;
    while (offset < length && bytes[offset] != '\0') ++offset;
    if (offset == length) return 1;
    memcpy(copy + start, bytes + start, offset - start);
    copy[offset] = '\0'; fields[count++] = copy + start; ++offset;
  }
  *field_count = count;
  return 0;
}

static int ct_plan_decimal(const char *value, size_t *result)
{
  size_t number = 0U, index;
  if (value == NULL || value[0] == '\0' || (value[0] == '0' && value[1] != '\0')) return 1;
  for (index = 0U; value[index] != '\0'; ++index) {
    if (value[index] < '0' || value[index] > '9' || number > (SIZE_MAX - (size_t)(value[index] - '0')) / 10U) return 1;
    number = number * 10U + (size_t)(value[index] - '0');
  }
  *result = number;
  return 0;
}

static int ct_plan_decode(const unsigned char *bytes, size_t length,
                          char **copy_result,
                          char *fields[CT_PLAN_FIELD_MAX],
                          size_t *fields_count,
                          struct ct_mount_plan_entry entries[CT_MOUNT_PLAN_MAX_ENTRIES],
                          size_t *entry_count)
{
  char *copy, digest[65];
  struct ct_mount_plan plan;
  struct ct_sha256 hash;
  unsigned char raw[32];
  size_t index;
  if (bytes == NULL || length == 0U || length > CT_MOUNT_PLAN_MAX_BYTES || bytes[length - 1U] != '\0') return 1;
  copy = malloc(length);
  if (copy == NULL || ct_plan_count(bytes, length, fields, copy, fields_count) != 0 || *fields_count < 7U || strcmp(fields[0], "ct-mount-plan-v1") != 0 ||
      strlen(fields[1]) != 64U || strspn(fields[1], "0123456789abcdef") != 64U || ct_plan_decimal(fields[6], entry_count) != 0 || *entry_count > CT_MOUNT_PLAN_MAX_ENTRIES || *fields_count != 7U + *entry_count * 5U) { free(copy); return 1; }
  for (index = 0U; index < *entry_count; ++index) entries[index] = (struct ct_mount_plan_entry){fields[7U + index * 5U], fields[8U + index * 5U], fields[9U + index * 5U], fields[10U + index * 5U], fields[11U + index * 5U]};
  plan = (struct ct_mount_plan){fields[2], fields[3], fields[4], fields[5], entries, *entry_count};
  if (ct_mount_plan_validate(&plan) != 0) { free(copy); return 1; }
  ct_sha256_init(&hash);
  for (index = 0U; index < *fields_count; ++index) if (index != 1U) ct_sha256_update(&hash, fields[index], strlen(fields[index]) + 1U);
  ct_sha256_final(&hash, raw); ct_sha256_hex(raw, digest);
  if (strcmp(digest, fields[1]) != 0) { free(copy); return 1; }
  *copy_result = copy;
  return 0;
}

int ct_mount_plan_parse(const unsigned char *bytes, size_t length)
{
  char *copy, *fields[CT_PLAN_FIELD_MAX];
  struct ct_mount_plan_entry entries[CT_MOUNT_PLAN_MAX_ENTRIES];
  size_t fields_count, entry_count;
  if (ct_plan_decode(bytes, length, &copy, fields, &fields_count, entries,
                     &entry_count) != 0) return 1;
  free(copy);
  return 0;
}

static int ct_plan_copy(char *output, size_t output_size, const char *value)
{
  const size_t length = value == NULL ? output_size : strlen(value);
  if (length >= output_size) return 1;
  memcpy(output, value, length + 1U);
  return 0;
}

int ct_mount_plan_visit(const unsigned char *bytes, size_t length,
                        struct ct_mount_plan_metadata *metadata,
                        ct_mount_plan_entry_visitor visitor, void *context)
{
  char *copy, *fields[CT_PLAN_FIELD_MAX];
  struct ct_mount_plan_entry entries[CT_MOUNT_PLAN_MAX_ENTRIES];
  size_t fields_count, entry_count, index;
  if (metadata == NULL || visitor == NULL ||
      ct_plan_decode(bytes, length, &copy, fields, &fields_count, entries,
                     &entry_count) != 0) return 1;
  if (ct_plan_copy(metadata->digest, sizeof(metadata->digest), fields[1]) != 0 ||
      ct_plan_copy(metadata->backend, sizeof(metadata->backend), fields[2]) != 0 ||
      ct_plan_copy(metadata->strategy, sizeof(metadata->strategy), fields[3]) != 0 ||
      ct_plan_copy(metadata->completeness, sizeof(metadata->completeness), fields[4]) != 0 ||
      ct_plan_copy(metadata->group_mode, sizeof(metadata->group_mode), fields[5]) != 0) {
    free(copy);
    return 1;
  }
  metadata->entry_count = entry_count;
  for (index = 0U; index < entry_count; ++index) {
    entries[index] = (struct ct_mount_plan_entry){fields[7U + index * 5U], fields[8U + index * 5U], fields[9U + index * 5U], fields[10U + index * 5U], fields[11U + index * 5U]};
    if (visitor(&entries[index], context) != 0) { free(copy); return 1; }
  }
  free(copy);
  return 0;
}

int ct_mount_plan_read(const char *path, struct ct_mount_plan_metadata *metadata,
                       ct_mount_plan_entry_visitor visitor, void *context)
{
  struct stat before, after, final;
  unsigned char *bytes;
  int descriptor = -1;
  int result;
  if (path == NULL || metadata == NULL || visitor == NULL ||
      (descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)) < 0 ||
      fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size < 1 ||
      (uintmax_t)before.st_size > CT_MOUNT_PLAN_MAX_BYTES) {
    if (descriptor >= 0) (void)close(descriptor);
    return 1;
  }
  bytes = malloc((size_t)before.st_size);
  if (bytes == NULL ||
      ct_plan_read_exact(descriptor, bytes, (size_t)before.st_size) != 0) {
    (void)close(descriptor);
    free(bytes);
    return 1;
  }
  #ifdef CT_STORAGE_TIMEOUT_TEST_SEAM
  {
    const char *pause = getenv("CT_MOUNT_PLAN_TEST_PAUSE_AFTER_READ");
    if (pause != NULL && pause[0] != '\0') {
      const struct timespec duration = {0, 100000000L};
      (void)nanosleep(&duration, NULL);
    }
  }
  #endif
  if (fstat(descriptor, &after) != 0 || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
    (void)close(descriptor);
    free(bytes);
    return 1;
  }
  if (close(descriptor) != 0 || stat(path, &final) != 0 ||
      !S_ISREG(final.st_mode) || final.st_dev != before.st_dev ||
      final.st_ino != before.st_ino || final.st_size != before.st_size ||
      final.st_mtim.tv_sec != before.st_mtim.tv_sec ||
      final.st_mtim.tv_nsec != before.st_mtim.tv_nsec) {
    free(bytes);
    return 1;
  }
  result = ct_mount_plan_visit(bytes, (size_t)before.st_size, metadata,
                               visitor, context);
  free(bytes);
  return result;
}

static int ct_plan_read_exact(int descriptor, unsigned char *bytes, size_t length)
{
  size_t used = 0U;
  while (used < length) {
    const ssize_t received = ct_storage_timeout_read(descriptor, bytes + used, length - used);
    if (received <= 0) return 1;
    used += (size_t)received;
  }
  return 0;
}

static int ct_plan_write_exact(int descriptor, const unsigned char *bytes, size_t length)
{
  size_t used = 0U;
  while (used < length) {
    const ssize_t written = ct_storage_timeout_write(descriptor, bytes + used, length - used);
    if (written <= 0) return 1;
    used += (size_t)written;
  }
  return 0;
}

static int ct_plan_existing(const char *path, const unsigned char *expected, size_t length)
{
  struct stat status, descriptor_status, final_status;
  unsigned char *actual;
  int descriptor, result;
  if (ct_storage_timeout_lstat(path, &status) != 0 || !S_ISREG(status.st_mode) || S_ISLNK(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 077U) != 0U || (size_t)status.st_size != length) return 1;
  descriptor = ct_storage_timeout_open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0);
  actual = malloc(length);
  if (descriptor < 0 || actual == NULL) { if (descriptor >= 0) (void)ct_storage_timeout_close(descriptor); free(actual); return 1; }
  result = ct_storage_timeout_fstat(descriptor, &descriptor_status) != 0 || !S_ISREG(descriptor_status.st_mode) ||
           descriptor_status.st_uid != status.st_uid || descriptor_status.st_dev != status.st_dev ||
           descriptor_status.st_ino != status.st_ino || descriptor_status.st_size != status.st_size ||
           ct_plan_read_exact(descriptor, actual, length) != 0 || ct_storage_timeout_close(descriptor) != 0 ||
           ct_storage_timeout_lstat(path, &final_status) != 0 || !S_ISREG(final_status.st_mode) ||
           S_ISLNK(final_status.st_mode) || final_status.st_uid != status.st_uid ||
           final_status.st_dev != status.st_dev || final_status.st_ino != status.st_ino ||
           final_status.st_size != status.st_size ||
           ct_mount_plan_parse(actual, length) != 0 || memcmp(actual, expected, length) != 0;
  free(actual); return result;
}

static int ct_plan_recover_temporary_files(const char *work, const char *digest)
{
  DIR *directory;
  struct dirent *entry;
  char prefix[80];
  size_t count = 0U;
  int written = snprintf(prefix, sizeof(prefix), ".tmp.%s.", digest);
  if (written < 0 || (size_t)written >= sizeof(prefix)) return 1;
  directory = ct_storage_timeout_opendir(work);
  if (directory == NULL) return 1;
  errno = 0;
  while ((entry = ct_storage_timeout_readdir(directory)) != NULL) {
    char path[4096];
    struct stat status;
    if (++count > 128U) { (void)ct_storage_timeout_closedir(directory); return 1; }
    if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) continue;
    written = snprintf(path, sizeof(path), "%s/%s", work, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(path) ||
        ct_storage_timeout_lstat(path, &status) != 0 || !S_ISREG(status.st_mode) || S_ISLNK(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 077U) != 0U || ct_storage_timeout_unlink(path) != 0) {
      (void)ct_storage_timeout_closedir(directory);
      return 1;
    }
  }
  return errno == 0 && ct_storage_timeout_closedir(directory) == 0 ? 0 : 1;
}

int ct_mount_plan_publish(const struct ct_mount_plan *plan, const char *state_root, char path[4096])
{
  unsigned char *bytes = NULL;
  char digest[65], work[4096], locks[4096], lock_path[4096] = "", temporary[4096] = "";
  size_t length;
  int lock_descriptor = -1, descriptor = -1, result = 1;
  struct stat lock_status, lock_descriptor_status;
  int written;
  if (state_root == NULL || path == NULL || strchr(state_root, ':') != NULL || strchr(state_root, ',') != NULL || strchr(state_root, '\n') != NULL ||
      ct_storage_ensure_private_directory(state_root) != 0 || ct_mount_plan_serialize(plan, &bytes, &length, digest) != 0) goto done;
  written = snprintf(path, 4096U, "%s/%s.manifest", state_root, digest);
  if (written < 0 || (size_t)written >= 4096U) goto done;
  written = snprintf(work, sizeof(work), "%s/.work", state_root);
  if (written < 0 || (size_t)written >= sizeof(work)) goto done;
  written = snprintf(locks, sizeof(locks), "%s/.locks", state_root);
  if (written < 0 || (size_t)written >= sizeof(locks) || ct_storage_ensure_private_directory(work) != 0 ||
      ct_storage_ensure_private_directory(locks) != 0) goto done;
  if (ct_plan_existing(path, bytes, length) == 0) { result = 0; goto done; }
  written = snprintf(lock_path, sizeof(lock_path), "%s/%s.lock", locks, digest);
  if (written < 0 || (size_t)written >= sizeof(lock_path)) goto done;
  lock_descriptor = ct_storage_timeout_open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_descriptor < 0 || ct_storage_timeout_lstat(lock_path, &lock_status) != 0 ||
      ct_storage_timeout_fstat(lock_descriptor, &lock_descriptor_status) != 0 || !S_ISREG(lock_status.st_mode) ||
      S_ISLNK(lock_status.st_mode) || lock_status.st_uid != geteuid() || (lock_status.st_mode & 077U) != 0U ||
      lock_descriptor_status.st_dev != lock_status.st_dev || lock_descriptor_status.st_ino != lock_status.st_ino ||
      ct_storage_timeout_flock_lock(lock_descriptor) != 0) goto done;
  if (ct_plan_existing(path, bytes, length) == 0) { result = 0; goto done; }
  if (ct_plan_recover_temporary_files(work, digest) != 0) goto done;
  written = snprintf(temporary, sizeof(temporary), "%s/.tmp.%s.%ld.%lu", work, digest, (long)getpid(), ++ct_plan_temporary_sequence);
  if (written < 0 || (size_t)written >= sizeof(temporary)) goto done;
  descriptor = ct_storage_timeout_open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0 || ct_plan_write_exact(descriptor, bytes, length) != 0 || ct_storage_timeout_fsync(descriptor) != 0 || ct_storage_timeout_close(descriptor) != 0) goto done;
  descriptor = -1;
  /* link(2) is atomic no-replace within this private filesystem. */
  if (ct_storage_timeout_link(temporary, path) != 0 && errno != EEXIST) goto done;
  if (ct_storage_timeout_unlink(temporary) != 0) goto done;
  if (ct_plan_existing(path, bytes, length) != 0) goto done;
  result = 0;
done:
  if (descriptor >= 0) (void)ct_storage_timeout_close(descriptor);
  if (result != 0 && temporary[0] != '\0') (void)ct_storage_timeout_unlink(temporary);
  if (lock_descriptor >= 0) { (void)ct_storage_timeout_flock_unlock(lock_descriptor); (void)ct_storage_timeout_close(lock_descriptor); }
  free(bytes);
  return result;
}

int ct_mount_plan_source_exposes_state(const char *source, const char *state_root)
{
  const size_t source_length = source == NULL ? 0U : strlen(source);
  const size_t root_length = state_root == NULL ? 0U : strlen(state_root);
  if (source == NULL || state_root == NULL || source[0] != '/' || state_root[0] != '/') return 1;
  return (strncmp(state_root, source, source_length) == 0 && (state_root[source_length] == '\0' || state_root[source_length] == '/')) ||
         (strncmp(source, state_root, root_length) == 0 && (source[root_length] == '\0' || source[root_length] == '/'));
}
