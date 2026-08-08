/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "host_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
  const char valid[] =
    "version = 1\n"
    "default_profile = \"host\"\n"
    "[profiles.host]\nroot = \"/host\"\nroot_access = \"inherit\"\nsemantics = \"full-root\"\n"
    "mount_plan = \"/.container-tools-mount-plan\"\ncwd_unmapped = \"error\"\npath = [\"/usr/bin\", \"/bin\"]\n"
    "environment_remove = [\"LD_PRELOAD\"]\n[profiles.host.environment]\nSLURM_CONF = \"/etc/slurm/slurm.conf\"\n"
    "[[profiles.host.projections]]\nvisible = \"/workspace\"\ntarget = \"/workspace\"\naccess = \"inherit\"\nrequired = true\n";
  static struct ct_host_profile profile;
  const char *const invalid[] = {
    "version = 2\ndefault_profile = \"x\"\n[profiles.x]\n",
    "version = 1\ndefault_profile = \"x\"\nunknown = 1\n[profiles.x]\n",
    "version = 1\ndefault_profile = \"x\"\n[profiles.x]\nroot=\"relative\"\nroot_access=\"inherit\"\nsemantics=\"full-root\"\ncwd_unmapped=\"error\"\npath=[\"/bin\"]\n",
    "version = 1\ndefault_profile = \"x\"\n[profiles.x]\nroot=\"/a/../b\"\nroot_access=\"inherit\"\nsemantics=\"full-root\"\ncwd_unmapped=\"error\"\npath=[\"/bin\"]\n",
    "version = 1\ndefault_profile = \"x\"\n[profiles.x]\nroot=\"/\"\nroot_access=\"inherit\"\nsemantics=\"full-root\"\ncwd_unmapped=\"error\"\npath=[]\n",
    "version = 1\ndefault_profile = \"x\"\n[profiles.x]\nroot=\"/\"\nroot_access=\"inherit\"\nsemantics=\"full-root\"\ncwd_unmapped=\"error\"\npath=[\"/bin\"]\n[profiles.x.environment]\nPATH=\"/bad\"\n",
    "version = 1\ndefault_profile = \"x\"\n[profiles.x]\nroot=\"/\"\nroot_access=\"inherit\"\nsemantics=\"full-root\"\ncwd_unmapped=\"error\"\npath=[\"/bin\"]\nenvironment_remove=[\"PATH\"]\n",
    "version = 1\ndefault_profile = \"x\"\n[profiles.x]\nroot=\"/\"\nroot_access=\"inherit\"\nsemantics=\"full-root\"\ncwd_unmapped=\"error\"\npath=[\"/bin\"]\nextra=true\n",
    "version = 1\ndefault_profile = \"x\"\n[profiles.x]\nroot=\"/\"\nroot_access=\"inherit\"\nsemantics=\"full-root\"\ncwd_unmapped=\"error\"\npath=[\"/safe:/bin\"]\n",
    "version = 1\ndefault_profile = \"missing\"\n[profiles.x]\nroot=\"/\"\nroot_access=\"inherit\"\nsemantics=\"full-root\"\ncwd_unmapped=\"error\"\npath=[\"/bin\"]\n",
    "version = 1\ndefault_profile = \"x\"\n[profiles.x]\nroot=\"/\"\nroot_access=\"inherit\"\nsemantics=\"full-root\"\ncwd_unmapped=\"error\"\npath=[\"/bin\"]\n[profiles.bad]\nroot=\"relative\"\n",
    NULL
  };
  char work[] = "/tmp/mkchad-v1/container-tools-c11/host-config.XXXXXX";
  char config[4096], link[4096];
  static char oversized[1048578];
  FILE *stream;
  size_t index;

  if (ct_host_config_parse(valid, sizeof(valid) - 1U, NULL, &profile) != CT_HOST_CONFIG_OK ||
      strcmp(profile.root, "/host") != 0 || profile.path_count != 2U ||
      profile.projection_count != 1U || profile.environment_remove_count != 1U ||
      profile.mount_plan_configured == 0) return 1;
  for (index = 0U; invalid[index] != NULL; ++index) {
    memset(&profile, 0x5a, sizeof(profile));
    if (ct_host_config_parse(invalid[index], strlen(invalid[index]), NULL,
                             &profile) !=
            CT_HOST_CONFIG_INVALID ||
        (unsigned char)profile.name[0] != 0x5aU) return 1;
  }
  memset(oversized, 'x', sizeof(oversized));
  oversized[sizeof(oversized) - 1U] = '\0';
  if (ct_host_config_parse(oversized, sizeof(oversized) - 1U, NULL, &profile) != CT_HOST_CONFIG_INVALID ||
      mkdtemp(work) == NULL ||
      snprintf(config, sizeof(config), "%s/config.toml", work) >=
          (int)sizeof(config) ||
      snprintf(link, sizeof(link), "%s/config-link.toml", work) >=
           (int)sizeof(link)) return 1;
  {
    static const char embedded[] = "version = 1\n\0default_profile = \"x\"\n";
    if (ct_host_config_parse(embedded, sizeof(embedded) - 1U, NULL,
                             &profile) != CT_HOST_CONFIG_INVALID) return 1;
  }
  stream = fopen(config, "w");
  if (stream == NULL || fputs(valid, stream) == EOF || fclose(stream) != 0 ||
      symlink(config, link) != 0 ||
      ct_host_config_load(link, NULL, &profile) != CT_HOST_CONFIG_OK ||
      setenv("CONTAINER_TOOLS_HOST_CONFIG", link, 1) != 0 ||
      ct_host_config_load(NULL, NULL, &profile) != CT_HOST_CONFIG_OK ||
      unsetenv("CONTAINER_TOOLS_HOST_CONFIG") != 0 || unlink(link) != 0 ||
      mkfifo(link, 0600) != 0 ||
      ct_host_config_load(link, NULL, &profile) != CT_HOST_CONFIG_INVALID ||
      unlink(link) != 0 || unlink(config) != 0 || rmdir(work) != 0) return 1;
  return 0;
}
