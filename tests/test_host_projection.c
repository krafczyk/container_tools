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
  char root[4096], cache[4096], mountinfo[4096], fake[4096], executable[4096], log[4096], config[4096], path[8192], contents[16384], cache_record[4096], cache_temporary[4096], other_temporary[4096];
  FILE *stream;
  DIR *directory;
  struct dirent *entry;
  struct ct_host_projection selection;
  size_t bytes;
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
       snprintf(mountinfo, sizeof(mountinfo), "%s/mountinfo", work) >= (int)sizeof(mountinfo)) return 1;
  stream = fopen(executable, "w");
  if (stream == NULL || fputs("#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"$CT_NATIVE_PROJECTION_LOG\"\nif [ \"$1\" = container ] && [ \"$2\" = inspect ]; then last=; for arg; do last=$arg; done; printf '%s\\n' \"$last\"; fi\nif [ \"${CT_NATIVE_PROJECTION_WEDGE:-}\" = \"$1\" ]; then trap '' TERM; (trap '' TERM; while :; do sleep 1; done) & wait; fi\n", stream) == EOF || fclose(stream) != 0 || chmod(executable, 0700) != 0) return 1;
  stream = fopen(mountinfo, "w");
  if (stream == NULL || fprintf(stream, "1 0 0:1 / / rw - ext4 root rw\n") < 0 || fclose(stream) != 0 ||
      setenv("CT_HOST_PROJECTION_SOURCE_ROOT", root, 1) != 0 || setenv("CT_HOST_PROJECTION_MOUNTINFO", mountinfo, 1) != 0 ||
        setenv("CT_HOST_PROJECTION_CACHE_ROOT", cache, 1) != 0 || setenv("CT_NATIVE_PROJECTION_LOG", log, 1) != 0 || setenv("PATH", path, 1) != 0 ||
        setenv("CT_RUNTIME_OPERATION_TIMEOUT", "0.05", 1) != 0 ||
       ct_host_projection_prepare("docker", "fixture-image", "required", 0, &selection) != 0 || strcmp(selection.strategy, "direct") != 0 ||
       strcmp(selection.completeness, "complete") != 0 || selection.entry_count != 1U || strcmp(selection.entries[0].destination, "/host") != 0) return 1;
  stream = fopen(log, "r");
  if (stream == NULL || (bytes = fread(contents, 1U, sizeof(contents) - 1U, stream)) == 0U || fclose(stream) != 0) return 1;
  contents[bytes] = '\0';
  if (strstr(contents, "create") == NULL || strstr(contents, "start") == NULL || strstr(contents, "rm") == NULL) return 1;
  if (ct_host_projection_prepare("docker", "fixture-image", "required", 0, &selection) != 0 || strcmp(selection.strategy, "direct") != 0 ||
        ct_host_projection_prepare("docker", "fixture-image", "required", 1, &selection) != 0 || strcmp(selection.strategy, "direct") != 0 ||
        ct_host_projection_prepare("docker", "fixture-image", "disabled", 0, &selection) != 0 || strcmp(selection.strategy, "none") != 0 ||
        strcmp(selection.completeness, "complete") != 0 || selection.entry_count != 0U) return 1;
  directory = opendir(cache);
  cache_record[0] = '\0';
  while (directory != NULL && (entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] != '.' && snprintf(cache_record, sizeof(cache_record), "%s/%s", cache, entry->d_name) >= (int)sizeof(cache_record)) return 1;
  }
  if (directory == NULL || closedir(directory) != 0 || cache_record[0] == '\0') return 1;
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
  if (stream == NULL || fputs("stale", stream) == EOF || fclose(stream) != 0 || chmod(cache_temporary, 0600) != 0) return 1;
  stream = fopen(other_temporary, "w");
  if (stream == NULL || fputs("active", stream) == EOF || fclose(stream) != 0 || chmod(other_temporary, 0600) != 0 ||
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
  stream = fopen(config, "w");
  if (stream == NULL || fputs("{\"currentContext\":\"remote-context\"}", stream) == EOF || fclose(stream) != 0 ||
       setenv("DOCKER_CONFIG", work, 1) != 0 || setenv("DOCKER_CONTEXT", "", 1) != 0 || ct_host_projection_endpoint_is_local("docker") != 0) return 1;
  return 0;
}
