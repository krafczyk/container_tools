/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "mount.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
  const char *file_tokens[] = {"--exclude-path", "/vector", "path", "--add-path",
                               "/literal;delimiter"};
  const char *extra_tokens[] = {"--exclude-path", "/extra", "path", "--add-path",
                                "/with,comma"};
  char output[256];

  if (ct_mount_format_args(file_tokens, 5U, extra_tokens, 5U, output, sizeof(output)) !=
          CT_MOUNT_OK ||
      strcmp(output, "--exclude-path /vector path /extra path --add-path /literal;delimiter /with,comma") != 0) {
    (void)fputs("mount argument formatting changed\n", stderr);
    return 1;
  }
  return 0;
}
