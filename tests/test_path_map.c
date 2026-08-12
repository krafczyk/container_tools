/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "path_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
  struct ct_path_map map;
  char cwd[CT_HOST_PATH_MAX];
  char visible[CT_HOST_PATH_MAX];
  static struct ct_host_profile profile;
  struct ct_host_environment_operation environment[CT_HOST_MAX_ENVIRONMENT + 1U];
  size_t environment_count;
  ct_path_map_init(&map);
  if (ct_path_map_set_root(&map, "/host") != 0 ||
      ct_path_map_add(&map, "/workspace", "/workspace", "inherit", 0U, 0U) != 0 ||
      ct_path_map_add(&map, "/workspace/source", "/src", "read-only", 1U, 1U) != 0 ||
      ct_path_map_add(&map, "/other", "/unrelated", "inherit", 1U, 0U) != 0 ||
      ct_path_map_add(&map, "/bad", "/", "inherit", 1U, 2U) == 0 ||
      ct_path_map_sort(&map) != 0 || strcmp(map.entries[0].target, "/workspace") != 0 ||
      strcmp(map.entries[1].target, "/unrelated") != 0 ||
      strcmp(map.entries[2].target, "/src") != 0 ||
      ct_path_map_cwd(&map, "/workspace/source/file", "error", cwd) != 0 ||
      strcmp(cwd, "/src/file") != 0 ||
      ct_path_map_cwd(&map, "/host/usr/bin", "error", cwd) != 0 ||
      strcmp(cwd, "/usr/bin") != 0) { ct_path_map_destroy(&map); return 1; }
  if (ct_path_map_visible(&map, "/src", visible) != 0 ||
      strcmp(visible, "/workspace/source") != 0 ||
      ct_path_map_visible(&map, "/src/include/file", visible) != 0 ||
      strcmp(visible, "/workspace/source/include/file") != 0 ||
      ct_path_map_visible(&map, "/usr/bin", visible) != 0 ||
      strcmp(visible, "/host/usr/bin") != 0) { ct_path_map_destroy(&map); return 2; }
  if (
      ct_path_map_cwd(&map, "/outside", "error", cwd) == 0 ||
      ct_path_map_cwd(&map, "/outside", "root", cwd) != 0 ||
      strcmp(cwd, "/") != 0) { ct_path_map_destroy(&map); return 1; }
  {
    const int result = ct_path_map_add(&map, "/duplicate", "/src", "inherit", 1U, 3U) != 0 ? 0 : 1;
    ct_path_map_destroy(&map);
    if (result != 0) return 3;
  }
  ct_path_map_init(&map);
  if (ct_path_map_set_root(&map, "/") != 0 ||
      ct_path_map_add(&map, "/", "/all", "inherit", 0U, 0U) != 0 ||
      ct_path_map_cwd(&map, "/some/path", "error", cwd) != 0 ||
      strcmp(cwd, "/all/some/path") != 0) return 4;
  ct_path_map_destroy(&map);
  memset(&profile, 0, sizeof(profile));
  strcpy(profile.path[0], "/usr/bin");
  strcpy(profile.path[1], "/bin");
  profile.path_count = 2U;
  strcpy(profile.environment_remove[0], "LD_PRELOAD");
  profile.environment_remove_count = 1U;
  strcpy(profile.environment[0].name, "SITE_CONFIG");
  strcpy(profile.environment[0].value, "/etc/site.conf");
  profile.environment_count = 1U;
  if (ct_path_map_environment(&profile, environment, &environment_count) != 0 ||
      environment_count != 3U || strcmp(environment[0].name, "LD_PRELOAD") != 0 ||
      environment[0].kind != CT_HOST_ENVIRONMENT_REMOVE ||
      environment[0].value[0] != '\0' || strcmp(environment[1].name, "PATH") != 0 ||
      environment[1].kind != CT_HOST_ENVIRONMENT_SET ||
      strcmp(environment[1].value, "/usr/bin:/bin") != 0 ||
      strcmp(environment[2].name, "SITE_CONFIG") != 0) return 5;
  {
    struct ct_mount_plan_metadata metadata;
    struct ct_path_map_manifest_context manifest;
    const struct ct_mount_plan_entry generated = {
      "generated-host-root", "/host", "/", "inherit", "non-recursive"};
    const struct ct_mount_plan_entry replay = {
      "explicit", "/workspace", "/workspace", "inherit", "runtime-default"};
    const struct ct_mount_plan_entry root_replay = {
      "explicit", "/", "/", "inherit", "runtime-default"};
    const struct ct_mount_plan_entry wrong_generated = {
      "generated-host-root", "/wrong", "/", "inherit", "non-recursive"};
    memset(&profile, 0, sizeof(profile));
    memset(&metadata, 0, sizeof(metadata));
    strcpy(profile.root, "/host");
    strcpy(profile.semantics, "full-root");
    strcpy(metadata.strategy, "direct");
    strcpy(metadata.completeness, "complete");
    ct_path_map_init(&map);
    ct_path_map_manifest_init(&manifest, &map, &profile);
    if (ct_path_map_manifest_entry(&generated, &manifest) != 0 ||
        ct_path_map_manifest_entry(&replay, &manifest) != 0 ||
        ct_path_map_manifest_eligible(&profile, &metadata, &manifest) != 0 ||
        ct_path_map_manifest_entry(&root_replay, &manifest) == 0 ||
        ct_path_map_manifest_entry(&wrong_generated, &manifest) == 0) return 6;
    strcpy(metadata.completeness, "partial");
    if (ct_path_map_manifest_eligible(&profile, &metadata, &manifest) == 0) return 7;
    ct_path_map_destroy(&map);
    strcpy(profile.root, "/repair");
    ct_path_map_init(&map);
    ct_path_map_manifest_init(&manifest, &map, &profile);
    if (ct_path_map_manifest_entry(&generated, &manifest) != 0 ||
        ct_path_map_manifest_eligible(&profile, &metadata, &manifest) == 0) return 8;
    strcpy(metadata.completeness, "complete");
    if (ct_path_map_manifest_eligible(&profile, &metadata, &manifest) != 0) return 9;
    ct_path_map_destroy(&map);
    {
      char host_root[] = "/tmp/mkchad-v1/container-tools-c11/path-map-host.XXXXXX";
      char usr[CT_HOST_PATH_MAX], usr_bin[CT_HOST_PATH_MAX], bin[CT_HOST_PATH_MAX], arbitrary[CT_HOST_PATH_MAX];
      struct ct_mount_plan_entry generated_remap = {
        "generated-host-root", bin, "/usr/bin", "inherit", "non-recursive"};
      struct ct_mount_plan_entry malformed_generated = {
        "generated-host-root", arbitrary, "/usr/bin", "inherit", "non-recursive"};
      if (mkdtemp(host_root) == NULL ||
          snprintf(usr, sizeof(usr), "%s/usr", host_root) >= (int)sizeof(usr) ||
          snprintf(usr_bin, sizeof(usr_bin), "%s/bin", usr) >= (int)sizeof(usr_bin) ||
          snprintf(bin, sizeof(bin), "%s/bin", host_root) >= (int)sizeof(bin) ||
          snprintf(arbitrary, sizeof(arbitrary), "%s/arbitrary", host_root) >=
              (int)sizeof(arbitrary) ||
          mkdir(usr, 0700) != 0 || mkdir(usr_bin, 0700) != 0 ||
          mkdir(arbitrary, 0700) != 0 || symlink("usr/bin", bin) != 0 ||
          setenv("CT_TEST_PATH_MAP_HOST_ROOT", host_root, 1) != 0) return 10;
      ct_path_map_init(&map);
      ct_path_map_manifest_init(&manifest, &map, &profile);
      if (ct_path_map_manifest_entry(&generated_remap, &manifest) != 0 ||
          ct_path_map_manifest_entry(&malformed_generated, &manifest) == 0 ||
          unsetenv("CT_TEST_PATH_MAP_HOST_ROOT") != 0) return 10;
      ct_path_map_destroy(&map);
    }
  }
  ct_path_map_init(&map);
  for (size_t entry = 0U; entry < CT_PATH_MAP_MAX_ENTRIES; ++entry) {
    char path[64];
    if (snprintf(path, sizeof(path), "/entry-%zu", entry) >=
            (int)sizeof(path) ||
        ct_path_map_add(&map, path, path, "inherit", 0U,
                        (unsigned int)entry) != 0) {
      ct_path_map_destroy(&map);
      return 11;
    }
  }
  if (ct_path_map_add(&map, "/excess", "/excess", "inherit", 0U, 0U) == 0) {
    ct_path_map_destroy(&map);
    return 11;
  }
  ct_path_map_destroy(&map);
  return 0;
}
