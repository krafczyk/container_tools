/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "host_projection.h"

#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
  char work[] = "/tmp/mkchad-v1/container-tools-c11/native-host-projection.XXXXXX";
  char root[4096], fallback_root[4096], empty_root[4096], blocked[4096], unresolved[4096], visible[4096], cache[4096], cache_with_slash[4096], mountinfo[4096], fake[4096], executable[4096], log[4096], config[4096], path[8192], contents[16384], cache_record[4096], cache_locks[4096], cache_lock[4096], cache_lock_target[4096], cache_temporary[4096], other_temporary[4096], unexpected[4096];
  FILE *stream;
  DIR *directory;
  struct dirent *entry;
  struct ct_host_projection selection;
  char cache_key[65];
  char selected_cache[4096];
  size_t bytes;
  if (setenv("CT_HOST_PROJECTION_HOSTNAME", "fixture-host", 1) != 0 ||
      setenv("CT_HOST_PROJECTION_EXECUTABLE", "/fixture/apptainer", 1) != 0 ||
      setenv("CT_HOST_PROJECTION_ENDPOINT", "local", 1) != 0 ||
      setenv("CT_HOST_PROJECTION_UID", "1000", 1) != 0 ||
      setenv("CT_HOST_PROJECTION_GID", "1000", 1) != 0 ||
      setenv("CT_HOST_PROJECTION_BOOT_ID", "fixture-boot", 1) != 0 ||
      setenv("CT_HOST_PROJECTION_GROUPS", "42 7", 1) != 0 ||
      ct_host_projection_cache_key("apptainer", "/fixture/image.sif", cache_key) != 0 ||
      strcmp(cache_key, "65e95b19f1cdbbefa28ea517a942172677e971143ccc6b825652fb46ebeb83c9") != 0 ||
      unsetenv("CT_HOST_PROJECTION_HOSTNAME") != 0 ||
      unsetenv("CT_HOST_PROJECTION_EXECUTABLE") != 0 ||
      unsetenv("CT_HOST_PROJECTION_ENDPOINT") != 0 ||
      unsetenv("CT_HOST_PROJECTION_UID") != 0 ||
      unsetenv("CT_HOST_PROJECTION_GID") != 0 ||
      unsetenv("CT_HOST_PROJECTION_BOOT_ID") != 0 ||
      unsetenv("CT_HOST_PROJECTION_GROUPS") != 0) return 1;
  (void)unsetenv("DOCKER_HOST");
  if (setenv("DOCKER_CONTEXT", "default", 1) != 0 || unsetenv("DOCKER_MACHINE_NAME") != 0) return 1;
  if (ct_host_projection_endpoint_is_local("docker") == 0 || ct_host_projection_source_is_eligible("/host", "ext4") != 0 ||
      ct_host_projection_source_is_eligible("/workspace", "ext4") == 0) return 1;
  if (setenv("DOCKER_HOST", "tcp://remote.invalid", 1) != 0 || ct_host_projection_endpoint_is_local("docker") != 0) return 1;
  if (unsetenv("DOCKER_HOST") != 0 || mkdtemp(work) == NULL ||
      snprintf(root, sizeof(root), "%s/root", work) >= (int)sizeof(root) || mkdir(root, 0700) != 0 ||
       snprintf(cache, sizeof(cache), "%s/cache", work) >= (int)sizeof(cache) ||
       snprintf(fake, sizeof(fake), "%s/fake", work) >= (int)sizeof(fake) || mkdir(fake, 0700) != 0 ||
       snprintf(executable, sizeof(executable), "%s/docker", fake) >= (int)sizeof(executable) ||
       snprintf(log, sizeof(log), "%s/runtime.log", work) >= (int)sizeof(log) ||
       snprintf(config, sizeof(config), "%s/config.json", work) >= (int)sizeof(config) ||
       snprintf(path, sizeof(path), "%s:%s", fake, getenv("PATH") == NULL ? "" : getenv("PATH")) >= (int)sizeof(path) ||
        snprintf(mountinfo, sizeof(mountinfo), "%s/mountinfo", work) >= (int)sizeof(mountinfo) ||
        unsetenv("CT_HOST_PROJECTION_CACHE_ROOT") != 0 || unsetenv("XDG_RUNTIME_DIR") != 0 ||
        setenv("XDG_CACHE_HOME", fake, 1) != 0 || setenv("HOME", root, 1) != 0 ||
        ct_host_projection_test_cache_root(selected_cache) != 0 ||
        snprintf(contents, sizeof(contents), "%s/container-tools/host-projection-v1", fake) >=
            (int)sizeof(contents) || strcmp(selected_cache, contents) != 0 ||
        unsetenv("XDG_CACHE_HOME") != 0 ||
        ct_host_projection_test_cache_root(selected_cache) != 0 ||
        snprintf(contents, sizeof(contents), "%s/.cache/container-tools/host-projection-v1", root) >=
            (int)sizeof(contents) || strcmp(selected_cache, contents) != 0) return 1;
  stream = fopen(executable, "w");
  if (stream == NULL || fputs("#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"$CT_NATIVE_PROJECTION_LOG\"\nif [ \"${CT_NATIVE_PROJECTION_FORCE_FALLBACK:-}\" = 1 ] && [ \"$1\" = create ]; then for arg; do case \"$arg\" in *\"source=$CT_HOST_PROJECTION_SOURCE_ROOT,target=/host$CT_HOST_PROJECTION_SOURCE_ROOT,\"*) exit 20;; esac; done; fi\nif [ \"$1\" = container ] && [ \"$2\" = inspect ]; then last=; for arg; do last=$arg; done; printf '%s\\n' \"$last\"; fi\nif [ \"${CT_NATIVE_PROJECTION_WEDGE:-}\" = \"$1\" ]; then trap '' TERM; (trap '' TERM; while :; do sleep 1; done) & wait; fi\n", stream) == EOF || fclose(stream) != 0 || chmod(executable, 0700) != 0) return 1;
  stream = fopen(mountinfo, "w");
  if (stream == NULL || fprintf(stream, "1 0 0:1 / / rw - ext4 root rw\n") < 0 || fclose(stream) != 0 ||
      setenv("CT_HOST_PROJECTION_SOURCE_ROOT", root, 1) != 0 || setenv("CT_HOST_PROJECTION_MOUNTINFO", mountinfo, 1) != 0 ||
         setenv("CT_HOST_PROJECTION_CACHE_ROOT", cache, 1) != 0 || setenv("CT_NATIVE_PROJECTION_LOG", log, 1) != 0 || setenv("PATH", path, 1) != 0 ||
         setenv("CT_RUNTIME_OPERATION_TIMEOUT", "0.05", 1) != 0 ||
        ct_host_projection_cache_clear() != 0 || access(cache, F_OK) == 0 ||
        ct_host_projection_prepare("docker", "fixture-image", "required", 0, &selection) != 0 || strcmp(selection.strategy, "direct") != 0 ||
       strcmp(selection.completeness, "complete") != 0 || selection.entry_count != 1U ||
       snprintf(contents, sizeof(contents), "/host%s", root) >= (int)sizeof(contents) ||
       strcmp(selection.entries[0].destination, contents) != 0) return 1;
  stream = fopen(log, "r");
  if (stream == NULL || (bytes = fread(contents, 1U, sizeof(contents) - 1U, stream)) == 0U || fclose(stream) != 0) return 1;
  contents[bytes] = '\0';
  if (strstr(contents, "create") == NULL || strstr(contents, "start") == NULL || strstr(contents, "rm") == NULL) return 1;
  directory = opendir(cache);
  cache_record[0] = '\0';
  while (directory != NULL && (entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] != '.' && snprintf(cache_record, sizeof(cache_record), "%s/%s", cache, entry->d_name) >= (int)sizeof(cache_record)) return 1;
  }
  if (directory == NULL || closedir(directory) != 0 || cache_record[0] == '\0' ||
      snprintf(cache_locks, sizeof(cache_locks), "%s/.locks", cache) >= (int)sizeof(cache_locks)) return 1;
  directory = opendir(cache_locks);
  cache_lock[0] = '\0';
  while (directory != NULL && (entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] != '.' && snprintf(cache_lock, sizeof(cache_lock), "%s/%s", cache_locks, entry->d_name) >= (int)sizeof(cache_lock)) return 1;
  }
  if (directory == NULL || closedir(directory) != 0 || cache_lock[0] == '\0' ||
      ct_host_projection_cache_clear() != 0 || access(cache_record, F_OK) == 0 ||
      access(cache_locks, F_OK) != 0 || access(cache_lock, F_OK) != 0) return 1;
  stream = fopen(log, "w");
  if (stream == NULL || fclose(stream) != 0 ||
      ct_host_projection_prepare("docker", "fixture-image", "required", 0, &selection) != 0 ||
      strcmp(selection.strategy, "direct") != 0) return 1;
  stream = fopen(log, "r");
  if (stream == NULL || (bytes = fread(contents, 1U, sizeof(contents) - 1U, stream)) == 0U ||
      fclose(stream) != 0) return 1;
  contents[bytes] = '\0';
  if (strstr(contents, "create") == NULL || access(cache_record, F_OK) != 0 ||
      snprintf(cache_lock_target, sizeof(cache_lock_target), "%s/dangling-lock-target", work) >=
          (int)sizeof(cache_lock_target) || unlink(cache_lock) != 0 ||
      symlink(cache_lock_target, cache_lock) != 0 ||
      ct_host_projection_prepare("docker", "fixture-image", "required", 0, &selection) == 0 ||
      access(cache_lock_target, F_OK) == 0 || unlink(cache_lock) != 0 ||
      ct_host_projection_prepare("docker", "fixture-image", "required", 0, &selection) != 0 ||
      chmod(cache, 0777) != 0 || chmod(cache_locks, 0777) != 0 ||
      chmod(cache_lock, 0666) != 0 || chmod(cache_record, 0666) != 0 ||
      setenv("CT_TEST_STORAGE_PRESENT_FOREIGN_UID", "1", 1) != 0) return 1;
  if (ct_host_projection_prepare("docker", "fixture-image", "required", 0, &selection) != 0 || strcmp(selection.strategy, "direct") != 0 ||
        ct_host_projection_prepare("docker", "fixture-image", "required", 1, &selection) != 0 || strcmp(selection.strategy, "direct") != 0 ||
        unsetenv("CT_TEST_STORAGE_PRESENT_FOREIGN_UID") != 0 ||
        ct_host_projection_prepare("docker", "fixture-image", "disabled", 0, &selection) != 0 || strcmp(selection.strategy, "none") != 0 ||
        strcmp(selection.completeness, "complete") != 0 || selection.entry_count != 0U) return 1;
  stream = fopen(cache_record, "w");
  if (stream == NULL || fputs("future-cache-record", stream) == EOF || fclose(stream) != 0 || chmod(cache_record, 0600) != 0 ||
       ct_host_projection_prepare("docker", "fixture-image", "required", 0, &selection) != 0 || strcmp(selection.strategy, "direct") != 0) return 1;
  stream = fopen(cache_record, "w");
  if (stream == NULL) return 1;
  for (bytes = 0U; bytes <= 1048576U; ++bytes) if (fputc('x', stream) == EOF) { (void)fclose(stream); return 1; }
  if (fclose(stream) != 0 || chmod(cache_record, 0600) != 0 ||
      ct_host_projection_prepare("docker", "fixture-image", "required", 0, &selection) != 0 || strcmp(selection.strategy, "direct") != 0) return 1;
  if (snprintf(cache_temporary, sizeof(cache_temporary), "%s.tmp.stale", cache_record) >= (int)sizeof(cache_temporary) ||
      snprintf(other_temporary, sizeof(other_temporary), "%s/other.tmp.active", cache) >= (int)sizeof(other_temporary)) return 1;
  stream = fopen(cache_temporary, "w");
  if (stream == NULL || fputs("stale", stream) == EOF || fclose(stream) != 0 || chmod(cache_temporary, 0666) != 0) return 1;
  stream = fopen(other_temporary, "w");
  if (stream == NULL || fputs("active", stream) == EOF || fclose(stream) != 0 || chmod(other_temporary, 0666) != 0 ||
      ct_host_projection_prepare("docker", "fixture-image", "required", 1, &selection) != 0 ||
      access(cache_temporary, F_OK) == 0 || access(other_temporary, F_OK) != 0) return 1;
  stream = fopen(log, "w");
  if (stream == NULL || fclose(stream) != 0 || setenv("CT_RUNTIME_CREATE_TIMEOUT", "0.05", 1) != 0 ||
      setenv("CT_NATIVE_PROJECTION_WEDGE", "create", 1) != 0 ||
      ct_host_projection_prepare("docker", "wedge-image", "required", 1, &selection) != 0 ||
      strcmp(selection.strategy, "none") != 0 || selection.entry_count != 0U) { (void)fputs("wedge selection failed\n", stderr); return 1; }
  stream = fopen(log, "r");
  if (stream == NULL || (bytes = fread(contents, 1U, sizeof(contents) - 1U, stream)) == 0U || fclose(stream) != 0) { (void)fputs("wedge log failed\n", stderr); return 1; }
  contents[bytes] = '\0';
  if (strstr(contents, "start") != NULL || strstr(contents, "container") == NULL || strstr(contents, "rm") == NULL) { (void)fputs("wedge cleanup contract failed\n", stderr); return 1; }
  if (unsetenv("CT_NATIVE_PROJECTION_WEDGE") != 0 || unsetenv("CT_RUNTIME_CREATE_TIMEOUT") != 0 ||
      unsetenv("CT_RUNTIME_OPERATION_TIMEOUT") != 0) return 1;
  if (snprintf(fallback_root, sizeof(fallback_root), "%s/fallback-root", work) >=
          (int)sizeof(fallback_root) ||
      snprintf(empty_root, sizeof(empty_root), "%s/empty-root", work) >=
          (int)sizeof(empty_root) ||
      snprintf(blocked, sizeof(blocked), "%s/blocked", fallback_root) >=
          (int)sizeof(blocked) ||
      snprintf(unresolved, sizeof(unresolved), "%s/unresolved", fallback_root) >=
          (int)sizeof(unresolved) ||
      snprintf(visible, sizeof(visible), "%s/visible", fallback_root) >=
          (int)sizeof(visible) ||
      mkdir(fallback_root, 0700) != 0 || mkdir(empty_root, 0700) != 0 ||
      mkdir(blocked, 0700) != 0 ||
      symlink("missing", unresolved) != 0 || mkdir(visible, 0700) != 0) return 1;
  stream = fopen(mountinfo, "w");
  if (stream == NULL ||
      fprintf(stream, "1 0 0:1 / / rw - ext4 root rw\n2 1 0:2 / %s/proc rw - proc proc rw\n", blocked) < 0 ||
      fclose(stream) != 0 ||
      setenv("CT_HOST_PROJECTION_SOURCE_ROOT", fallback_root, 1) != 0 ||
      setenv("CT_NATIVE_PROJECTION_FORCE_FALLBACK", "1", 1) != 0 ||
      setenv("CT_TEST_STORAGE_OPENDIR_FAIL", blocked, 1) != 0 ||
      ct_host_projection_prepare("docker", "fallback-image", "required", 1,
                                 &selection) != 0 ||
      strcmp(selection.strategy, "fallback") != 0 ||
      strcmp(selection.completeness, "complete") != 0 ||
      selection.entry_count != 1U ||
      strcmp(selection.entries[0].source, visible) != 0) return 1;
  if (setenv("CT_TEST_STORAGE_STAT_ENOTCONN", visible, 1) != 0 ||
      ct_host_projection_prepare("docker", "fallback-image", "required", 0,
                                 &selection) != 0 ||
      strcmp(selection.strategy, "fallback") != 0 ||
      strcmp(selection.completeness, "partial") != 0 ||
      selection.entry_count != 0U) {
    (void)fprintf(stderr,
                  "disconnected cached source survived: strategy=%s completeness=%s entries=%zu\n",
                  selection.strategy, selection.completeness,
                  selection.entry_count);
    return 1;
  }
  if (unsetenv("CT_TEST_STORAGE_STAT_ENOTCONN") != 0 || rmdir(visible) != 0 ||
      setenv("CT_TEST_STORAGE_OPENDIR_TIMEOUT", "1", 1) != 0 ||
      ct_host_projection_prepare("docker", "fallback-timeout-image", "auto", 1,
                                 &selection) != 0 ||
      strcmp(selection.strategy, "none") != 0 ||
      unsetenv("CT_TEST_STORAGE_OPENDIR_TIMEOUT") != 0 ||
      setenv("CT_HOST_PROJECTION_SOURCE_ROOT", empty_root, 1) != 0 ||
      ct_host_projection_prepare("docker", "empty-fallback-image", "auto", 1,
                                 &selection) != 0 ||
      strcmp(selection.strategy, "none") != 0 ||
      unsetenv("CT_TEST_STORAGE_OPENDIR_FAIL") != 0 ||
      unsetenv("CT_NATIVE_PROJECTION_FORCE_FALLBACK") != 0 ||
      unlink(other_temporary) != 0 ||
      snprintf(cache_with_slash, sizeof(cache_with_slash), "%s/", cache) >=
          (int)sizeof(cache_with_slash)) return 1;
  stream = fopen(cache_temporary, "w");
  if (stream == NULL || fputs("stale", stream) == EOF || fclose(stream) != 0 ||
      setenv("CT_HOST_PROJECTION_CACHE_ROOT", cache_with_slash, 1) != 0 ||
      ct_host_projection_cache_clear() != 0 || access(cache_record, F_OK) == 0 ||
      access(cache_temporary, F_OK) == 0 ||
      setenv("CT_HOST_PROJECTION_CACHE_ROOT", cache, 1) != 0 ||
      snprintf(unexpected, sizeof(unexpected), "%s/unexpected", cache) >=
          (int)sizeof(unexpected)) return 1;
  stream = fopen(unexpected, "w");
  if (stream == NULL || fputs("keep", stream) == EOF || fclose(stream) != 0 ||
      ct_host_projection_cache_clear() == 0 || access(unexpected, F_OK) != 0 ||
      unlink(unexpected) != 0 || ct_host_projection_cache_clear() != 0) return 1;
  stream = fopen(config, "w");
   if (stream == NULL || fputs("{\"currentContext\":\"remote-context\"}", stream) == EOF || fclose(stream) != 0 ||
        setenv("DOCKER_CONFIG", work, 1) != 0 || unsetenv("DOCKER_CONTEXT") != 0 || ct_host_projection_endpoint_is_local("docker") != 0) return 1;
  return 0;
}
