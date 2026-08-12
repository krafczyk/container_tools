/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "mount_plan.h"
#include "sha256.h"

#include <fcntl.h>
#include <sys/file.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct visit_context { size_t count; int saw_explicit; };

static int visit_entry(const struct ct_mount_plan_entry *entry, void *opaque)
{
  struct visit_context *context = opaque;
  ++context->count;
  if (strcmp(entry->role, "explicit") == 0 &&
      strcmp(entry->caller_path, "/workspace") == 0) {
    context->saw_explicit = 1;
  }
  return 0;
}

int main(void)
{
  const struct ct_mount_plan_entry entries[] = {
    {"explicit", "/workspace", "/workspace", "inherit", "runtime-default"},
    {"bootstrap-internal", "/.container-tools-bootstrap", "/.container-tools-bootstrap", "read-only", "runtime-default"},
  };
  const struct ct_mount_plan plan = {"docker", "none", "complete", "primary-only", entries, 2U};
  unsigned char *bytes = NULL;
  size_t length = 0U;
  char digest[65];
  struct ct_sha256 hash;
  unsigned char raw[32];
  char golden[65];
  char work[] = "/tmp/mkchad-v1/container-tools-c11/native-mount-plan.XXXXXX";
  char state[4096], first[4096], second[4096], stale[4096], keep[4096], alternate_digest[65], other_stale[4096], lock[4096], link[4096], future[4096], mismatch[4096], malformed_cache[4096], state_override[4096], resolved_state[4096];
  FILE *stream;
  struct ct_mount_plan_metadata metadata;
  struct visit_context visited = {0U, 0};
  const struct ct_mount_plan_entry alternate_entries[] = {
    {"explicit", "/alternate", "/alternate", "inherit", "runtime-default"},
  };
  const struct ct_mount_plan alternate = {"docker", "none", "complete", "primary-only", alternate_entries, 1U};

  if (ct_mount_plan_validate(&plan) != 0 || ct_mount_plan_serialize(&plan, &bytes, &length, digest) != 0 ||
      length == 0U || ct_mount_plan_parse(bytes, length) != 0) {
    free(bytes);
    return 1;
  }
  bytes[length - 2U] ^= 1U;
  if (ct_mount_plan_parse(bytes, length) == 0) { free(bytes); return 1; }
  free(bytes);
  ct_sha256_init(&hash);
  ct_sha256_update(&hash, "abc", 3U);
  ct_sha256_final(&hash, raw);
  ct_sha256_hex(raw, golden);
  if (strcmp(golden, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) return 1;
  if (ct_mount_plan_serialize(&alternate, &bytes, &length, alternate_digest) != 0) return 1;
  free(bytes); bytes = NULL;
  if (mkdtemp(work) == NULL || snprintf(state, sizeof(state), "%s/state", work) >= (int)sizeof(state) ||
      snprintf(state_override, sizeof(state_override), "%s/", state) >=
          (int)sizeof(state_override) ||
      setenv("CT_MOUNT_PLAN_STATE_ROOT", state_override, 1) != 0 ||
      ct_mount_plan_state_root(resolved_state) != 0 ||
      strcmp(resolved_state, state) != 0 ||
      unsetenv("CT_MOUNT_PLAN_STATE_ROOT") != 0 ||
      ct_mount_plan_publish(&plan, state, first) != 0 || ct_mount_plan_publish(&plan, state, second) != 0 ||
      strcmp(first, second) != 0 || lstat(first, &(struct stat){0}) != 0 ||
       snprintf(stale, sizeof(stale), "%s/.work/.tmp.%s.interrupted", state, alternate_digest) >= (int)sizeof(stale) ||
       snprintf(other_stale, sizeof(other_stale), "%s/.work/.tmp.%s.active", state, digest) >= (int)sizeof(other_stale) ||
       snprintf(lock, sizeof(lock), "%s/.locks/%s.lock", state, alternate_digest) >= (int)sizeof(lock) ||
       snprintf(keep, sizeof(keep), "%s/.work/keep", state) >= (int)sizeof(keep) ||
        snprintf(link, sizeof(link), "%s/manifest-link", work) >= (int)sizeof(link) ||
        snprintf(future, sizeof(future), "%s/future.manifest", work) >= (int)sizeof(future) ||
        snprintf(mismatch, sizeof(mismatch), "%s/mismatch.manifest", work) >= (int)sizeof(mismatch) ||
       symlink(first, link) != 0 ||
       ct_mount_plan_read(link, &metadata, visit_entry, &visited) != 0 ||
       strcmp(metadata.grammar, "ct-mount-plan-v1") != 0 ||
       strcmp(metadata.digest, digest) != 0 || strcmp(metadata.backend, "docker") != 0 ||
         strcmp(metadata.strategy, "none") != 0 || metadata.entry_count != 2U ||
         visited.count != 2U || visited.saw_explicit == 0 || unlink(link) != 0 ||
         mkfifo(link, 0600) != 0 ||
          ct_mount_plan_read(link, &metadata, visit_entry, &visited) == 0 ||
          unlink(link) != 0 ||
          ct_mount_plan_read_status(future, &metadata, visit_entry, &visited) != CT_MOUNT_PLAN_READ_ABSENT) return 1;
#ifdef CT_STORAGE_TIMEOUT_TEST_SEAM
  if (setenv("CT_TEST_STORAGE_TIMEOUT_FORCE", "1", 1) != 0 ||
      ct_mount_plan_read_status(first, &metadata, visit_entry, &visited) !=
          CT_MOUNT_PLAN_READ_IO ||
      unsetenv("CT_TEST_STORAGE_TIMEOUT_FORCE") != 0) return 1;
#endif
  {
    const unsigned char future_bytes[] = "ct-mount-plan-v2\0";
    int descriptor = open(future, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0 || write(descriptor, future_bytes, sizeof(future_bytes)) != (ssize_t)sizeof(future_bytes) ||
        close(descriptor) != 0 || ct_mount_plan_read_status(future, &metadata, visit_entry, &visited) != CT_MOUNT_PLAN_READ_FUTURE ||
        strcmp(metadata.grammar, "ct-mount-plan-v2") != 0 ||
        unlink(future) != 0 || ct_mount_plan_serialize(&plan, &bytes, &length, digest) != 0) return 1;
    bytes[strlen("ct-mount-plan-v1") + 1U] = bytes[strlen("ct-mount-plan-v1") + 1U] == '0' ? '1' : '0';
    descriptor = open(mismatch, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0 || write(descriptor, bytes, length) != (ssize_t)length || close(descriptor) != 0 ||
        ct_mount_plan_read_status(mismatch, &metadata, visit_entry, &visited) != CT_MOUNT_PLAN_READ_DIGEST_MISMATCH ||
        unlink(mismatch) != 0) return 1;
    free(bytes); bytes = NULL;
  }
  {
    const pid_t writer = fork();
    int writer_status;
    if (writer < 0 || setenv("CT_MOUNT_PLAN_TEST_PAUSE_AFTER_READ", "1", 1) != 0) return 1;
    if (writer == 0) {
      const struct timespec pause = {0, 20000000L};
      const int descriptor = open(first, O_WRONLY | O_CLOEXEC);
      (void)nanosleep(&pause, NULL);
      if (descriptor < 0 || pwrite(descriptor, "X", 1U, 0) != 1 ||
          fsync(descriptor) != 0 || close(descriptor) != 0) _exit(1);
      _exit(0);
    }
    visited = (struct visit_context){0U, 0};
    if (ct_mount_plan_read_status(first, &metadata, visit_entry, &visited) != CT_MOUNT_PLAN_READ_CHANGED ||
        waitpid(writer, &writer_status, 0) != writer ||
        !WIFEXITED(writer_status) || WEXITSTATUS(writer_status) != 0 ||
        unsetenv("CT_MOUNT_PLAN_TEST_PAUSE_AFTER_READ") != 0) return 1;
  }
  stream = fopen(stale, "w");
  if (stream == NULL || fputs("partial", stream) == EOF || fclose(stream) != 0 || chmod(stale, 0600) != 0) return 1;
  stream = fopen(other_stale, "w");
  if (stream == NULL || fputs("active", stream) == EOF || fclose(stream) != 0 || chmod(other_stale, 0600) != 0) return 1;
  if (stream == NULL || (stream = fopen(keep, "w")) == NULL || fputs("keep", stream) == EOF || fclose(stream) != 0 ||
       ct_mount_plan_publish(&alternate, state, second) != 0 || access(stale, F_OK) == 0 || access(other_stale, F_OK) != 0 || access(keep, F_OK) != 0) return 1;
  if (snprintf(second, sizeof(second), "%s/%s.manifest", state, alternate_digest) >= (int)sizeof(second) || unlink(second) != 0) return 1;
  stream = fopen(stale, "w");
  if (stream == NULL || fputs("bad", stream) == EOF || fclose(stream) != 0 || chmod(stale, 0644) != 0 ||
      ct_mount_plan_publish(&alternate, state, second) == 0 || access(lock, F_OK) != 0) return 1;
  if (unlink(stale) != 0 || unlink(keep) != 0 || chmod(other_stale, 0600) != 0 ||
      ct_mount_plan_clear(state) != 0 || access(state, F_OK) != 0 ||
      ct_mount_plan_clear(state) != 0 ||
      snprintf(malformed_cache, sizeof(malformed_cache), "%s/malformed-cache", work) >=
          (int)sizeof(malformed_cache) || ct_mount_plan_clear(malformed_cache) != 0 ||
      mkdir(malformed_cache, 0700) != 0 ||
      snprintf(first, sizeof(first), "%s/unexpected", malformed_cache) >= (int)sizeof(first) ||
      (stream = fopen(first, "w")) == NULL || fputs("bad", stream) == EOF ||
      fclose(stream) != 0 || chmod(first, 0600) != 0 ||
      ct_mount_plan_clear(malformed_cache) == 0 || access(first, F_OK) != 0 ||
      unlink(first) != 0 || rmdir(malformed_cache) != 0) return 1;
#ifdef CT_STORAGE_TIMEOUT_TEST_SEAM
  if (mkdir(malformed_cache, 0700) != 0 ||
      setenv("CT_TEST_STORAGE_TIMEOUT_FORCE", "1", 1) != 0 ||
      ct_mount_plan_clear(malformed_cache) == 0 || access(malformed_cache, F_OK) != 0 ||
      unsetenv("CT_TEST_STORAGE_TIMEOUT_FORCE") != 0 || rmdir(malformed_cache) != 0) return 1;
#endif
  if (snprintf(stale, sizeof(stale), "%s/.work", state) >= (int)sizeof(stale) ||
      snprintf(keep, sizeof(keep), "%s/.tmp.%064d.x", stale, 0) >=
          (int)sizeof(keep) ||
      (stream = fopen(keep, "w")) == NULL || fclose(stream) != 0 ||
      chmod(keep, 0600) != 0 || ct_mount_plan_clear(state) != 0 ||
      access(keep, F_OK) == 0 || access(stale, F_OK) != 0 ||
      snprintf(keep, sizeof(keep), "%s/.lock", stale) >= (int)sizeof(keep) ||
      access(keep, F_OK) != 0 ||
      ct_mount_plan_publish(&plan, state, first) != 0 ||
      snprintf(lock, sizeof(lock), "%s/.locks/.cache.lock", state) >=
          (int)sizeof(lock)) return 1;
  {
    int ready[2], release[2], child_status;
    pid_t child;
    char byte;
    if (pipe(ready) != 0 || pipe(release) != 0 || (child = fork()) < 0) return 1;
    if (child == 0) {
      const int descriptor = open(lock, O_RDWR | O_CLOEXEC);
      (void)close(ready[0]); (void)close(release[1]);
      if (descriptor < 0 || flock(descriptor, LOCK_SH) != 0 || write(ready[1], "1", 1U) != 1 ||
          read(release[0], &byte, 1U) != 1 || close(descriptor) != 0) _exit(1);
      _exit(0);
    }
    (void)close(ready[1]); (void)close(release[0]);
    if (read(ready[0], &byte, 1U) != 1 || close(ready[0]) != 0 ||
        setenv("CT_RUNTIME_STORAGE_TIMEOUT", "0.1", 1) != 0 ||
        ct_mount_plan_clear(state) == 0 || access(first, F_OK) != 0 ||
        unsetenv("CT_RUNTIME_STORAGE_TIMEOUT") != 0 || write(release[1], "1", 1U) != 1 ||
        close(release[1]) != 0 || waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0 ||
        ct_mount_plan_clear(state) != 0) return 1;
  }
  {
    int ready[2], release[2], child_status;
    pid_t child;
    char byte;
    if (pipe(ready) != 0 || pipe(release) != 0 || (child = fork()) < 0) return 1;
    if (child == 0) {
      const int descriptor = open(lock, O_RDWR | O_CLOEXEC);
      (void)close(ready[0]); (void)close(release[1]);
      if (descriptor < 0 || flock(descriptor, LOCK_EX) != 0 || write(ready[1], "1", 1U) != 1 ||
          read(release[0], &byte, 1U) != 1 || close(descriptor) != 0) _exit(1);
      _exit(0);
    }
    (void)close(ready[1]); (void)close(release[0]);
    if (read(ready[0], &byte, 1U) != 1 || close(ready[0]) != 0 ||
        setenv("CT_RUNTIME_STORAGE_TIMEOUT", "0.1", 1) != 0 ||
        ct_mount_plan_publish(&plan, state, first) == 0 || access(first, F_OK) == 0 ||
        unsetenv("CT_RUNTIME_STORAGE_TIMEOUT") != 0 || write(release[1], "1", 1U) != 1 ||
        close(release[1]) != 0 || waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0 ||
        ct_mount_plan_publish(&plan, state, first) != 0 ||
        ct_mount_plan_clear(state) != 0) return 1;
  }
  if (ct_mount_plan_publish(&plan, state, first) != 0 ||
      snprintf(lock, sizeof(lock), "%s/.locks/%s.lock", state, digest) >=
          (int)sizeof(lock) || chmod(lock, 0644) != 0 ||
      snprintf(stale, sizeof(stale), "%s/.work/.body.legacy", state) >=
          (int)sizeof(stale) ||
      snprintf(keep, sizeof(keep), "%s/.work/.candidate.legacy", state) >=
          (int)sizeof(keep) ||
      snprintf(second, sizeof(second), "%s/.%s.tmp.legacy", state, digest) >=
          (int)sizeof(second) ||
      (stream = fopen(stale, "w")) == NULL || fclose(stream) != 0 ||
      chmod(stale, 0600) != 0 || (stream = fopen(keep, "w")) == NULL ||
      fclose(stream) != 0 || chmod(keep, 0600) != 0 ||
      (stream = fopen(second, "w")) == NULL || fclose(stream) != 0 ||
      chmod(second, 0600) != 0 || chmod(first, 0600) != 0 ||
      ct_mount_plan_clear(state) != 0 || access(stale, F_OK) == 0 ||
      access(keep, F_OK) == 0 || access(second, F_OK) == 0 ||
      access(first, F_OK) == 0 || access(lock, F_OK) == 0) return 1;
  if (snprintf(keep, sizeof(keep), "%s/.work/.lock", state) >=
          (int)sizeof(keep) ||
      chmod(keep, 0644) != 0 || ct_mount_plan_publish(&plan, state, first) != 0) return 1;
  {
    int ready[2], release[2], child_status;
    pid_t child;
    char byte;
    if (pipe(ready) != 0 || pipe(release) != 0 || (child = fork()) < 0) return 1;
    if (child == 0) {
      const int descriptor = open(keep, O_RDWR | O_CLOEXEC);
      (void)close(ready[0]); (void)close(release[1]);
      if (descriptor < 0 || flock(descriptor, LOCK_EX) != 0 || write(ready[1], "1", 1U) != 1 ||
          read(release[0], &byte, 1U) != 1 || close(descriptor) != 0) _exit(1);
      _exit(0);
    }
    (void)close(ready[1]); (void)close(release[0]);
    if (read(ready[0], &byte, 1U) != 1 || close(ready[0]) != 0 ||
        setenv("CT_RUNTIME_STORAGE_TIMEOUT", "0.1", 1) != 0 ||
        ct_mount_plan_clear(state) == 0 || access(first, F_OK) != 0 ||
        unsetenv("CT_RUNTIME_STORAGE_TIMEOUT") != 0 || write(release[1], "1", 1U) != 1 ||
        close(release[1]) != 0 || waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) return 1;
  }
  if (ct_mount_plan_clear(state) != 0 ||
      access(keep, F_OK) != 0) return 1;
  return ct_mount_plan_source_exposes_state("/tmp", "/tmp/state") != 0 ? 0 : 1;
}
