/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "instance_profile_manifest.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int finalize_profile(struct ct_instance_profile_manifest *profile,
                            char instance_name[40])
{
  return ct_instance_profile_manifest_profile_digest(profile,
                                                     profile->profile_digest) != 0 ||
         snprintf(instance_name, 40U, "mkchad-%.32s",
                  profile->profile_digest) >= 40;
}

int main(void)
{
  const char *groups[] = {"10", "11", "12", "13", "14", "15", "16", "17", "18", "19"};
  char instance_name[40];
  struct ct_instance_profile_manifest_mount mounts[] = {
      {"--bind", "/host/data:/data", "explicit", "inherit", "runtime-default",
       0, 1, "/host/data", "device=1,inode=2"},
      {"--mount", "/host:/host", "generated-host-root", "inherit", "runtime-default",
       1, 1, "/host", "absent"},
  };
  struct ct_instance_profile_manifest profile = {
       "", "", instance_name, "singularity", "ct-instance-profile-v4", "absent", "1000", "1000",
      "host", "/home/user", "/tmp/instances", "/images/image.sif", "image-id",
      "absent", "absent", "ct-host-projection-profile-v1",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
       "native-inherited", groups, 10U,
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      mounts, 2U};
  unsigned char *bytes = NULL;
  size_t length = 0U;

  if (finalize_profile(&profile, instance_name) != 0 ||
      ct_instance_profile_manifest_validate(&profile) != 0 ||
      ct_instance_profile_manifest_serialize(&profile, &bytes, &length) != 0 ||
      ct_instance_profile_manifest_parse(bytes, length) != 0) {
    free(bytes);
    return 1;
  }
  bytes[length - 2U] ^= 1U;
  if (ct_instance_profile_manifest_parse(bytes, length) == 0) {
    free(bytes);
    return 1;
  }
  free(bytes);
  {
    char root[] = "/tmp/mkchad-v1/profile-manifest-publish.XXXXXX";
    char manifest_path[4096];
    char profiles_path[4096];
    struct stat status;
    struct ct_instance_profile_manifest readback;
    if (mkdtemp(root) == NULL || chmod(root, 0700) != 0 ||
        ct_instance_profile_manifest_publish(root, &profile, manifest_path) != 0 ||
        stat(manifest_path, &status) != 0 || (status.st_mode & 0777U) != 0600U ||
        snprintf(profiles_path, sizeof(profiles_path), "%s/profiles", root) >=
            (int)sizeof(profiles_path) || chmod(root, 0750) != 0 ||
        chmod(profiles_path, 0770) != 0 || chmod(manifest_path, 0660) != 0 ||
        setenv("CT_TEST_STORAGE_PRESENT_FOREIGN_UID", "1", 1) != 0 ||
        ct_instance_profile_manifest_publish(root, &profile, manifest_path) != 0 ||
        ct_instance_profile_manifest_read_private(root, profile.profile_digest,
                                                  &readback) != CT_INSTANCE_PROFILE_MANIFEST_READ_OK ||
        unsetenv("CT_TEST_STORAGE_PRESENT_FOREIGN_UID") != 0) return 1;
    ct_instance_profile_manifest_destroy(&readback);
  }
  {
    const unsigned char future[] = "ct-instance-profile-v2\0";
    struct ct_instance_profile_manifest parsed;
    unsigned char *oversized = calloc(CT_INSTANCE_PROFILE_MANIFEST_MAX_BYTES + 1U, 1U);
    if (ct_instance_profile_manifest_parse((const unsigned char *)"bad", 3U) == 0 ||
        ct_instance_profile_manifest_parse(future, sizeof(future)) == 0 ||
        ct_instance_profile_manifest_parse_read(future, sizeof(future),
                                                &parsed) !=
            CT_INSTANCE_PROFILE_MANIFEST_READ_FUTURE ||
        oversized == NULL ||
        ct_instance_profile_manifest_parse(oversized, CT_INSTANCE_PROFILE_MANIFEST_MAX_BYTES + 1U) == 0) {
      free(oversized);
      return 1;
    }
    free(oversized);
  }
  instance_name[7] = 'x';
  if (ct_instance_profile_manifest_validate(&profile) == 0) return 1;
  instance_name[7] = profile.profile_digest[0];
  profile.profile_grammar = "ct-instance-profile-v3";
  if (ct_instance_profile_manifest_validate(&profile) == 0) return 1;
  profile.profile_grammar = "ct-instance-profile-v4";
  profile.hostname = "other-host";
  if (ct_instance_profile_manifest_validate(&profile) == 0) return 1;
  profile.hostname = "host";
  profile.runtime_argument_digest = "raw-runtime-argument";
  if (ct_instance_profile_manifest_validate(&profile) == 0) return 1;
  profile.runtime_argument_digest = NULL;
  if (ct_instance_profile_manifest_validate(&profile) == 0) return 1;
  return 0;
}
