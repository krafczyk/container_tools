/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "mount.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
  const char *file_tokens[] = {"--exclude-path", "/vector", "path", "--add-path",
                               "/literal;delimiter"};
  const char *extra_tokens[] = {"--exclude-path", "/extra", "path", "--add-path",
                                "/with,comma"};
  char config[] = "/tmp/mkchad-v1/container-tools-c11/mount-config.XXXXXX";
  char paths[CT_MOUNT_MAX_PATHS][CT_MOUNT_PATH_MAX];
  char output[256];
  size_t path_count = 1U;
  int descriptor;
  FILE *stream;

  if (ct_mount_format_args(file_tokens, 5U, extra_tokens, 5U, output, sizeof(output)) !=
          CT_MOUNT_OK ||
      strcmp(output, "--exclude-path /vector path /extra path --add-path /literal;delimiter /with,comma") != 0) {
    (void)fputs("mount argument formatting changed\n", stderr);
    return 1;
  }
  descriptor = mkstemp(config);
  stream = descriptor < 0 ? NULL : fdopen(descriptor, "w");
  if (stream == NULL || fputs("--add-path /host/path:/container/path\n", stream) == EOF ||
      fclose(stream) != 0 || setenv("CT_MOUNT_CFG", config, 1) != 0 ||
      ct_mount_collect_environment(paths, &path_count) == 0 || path_count != 0U ||
      unlink(config) != 0) {
    (void)fputs("persistent mount configuration accepted a remapped --add-path\n", stderr);
    return 1;
  }
  return 0;
}
