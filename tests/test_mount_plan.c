/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "mount_plan.h"
#include "sha256.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
  char state[4096], first[4096], second[4096], stale[4096], keep[4096], alternate_digest[65], other_stale[4096], lock[4096];
  FILE *stream;
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
      ct_mount_plan_publish(&plan, state, first) != 0 || ct_mount_plan_publish(&plan, state, second) != 0 ||
      strcmp(first, second) != 0 || lstat(first, &(struct stat){0}) != 0 ||
       snprintf(stale, sizeof(stale), "%s/.work/.tmp.%s.interrupted", state, alternate_digest) >= (int)sizeof(stale) ||
       snprintf(other_stale, sizeof(other_stale), "%s/.work/.tmp.%s.active", state, digest) >= (int)sizeof(other_stale) ||
       snprintf(lock, sizeof(lock), "%s/.locks/%s.lock", state, alternate_digest) >= (int)sizeof(lock) ||
       snprintf(keep, sizeof(keep), "%s/.work/keep", state) >= (int)sizeof(keep)) return 1;
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
  return ct_mount_plan_source_exposes_state("/tmp", "/tmp/state") != 0 ? 0 : 1;
}
