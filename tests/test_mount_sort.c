/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "../src/mount.c"

#include <stdio.h>
#include <string.h>

int main(void)
{
  char candidates[4][CT_MOUNT_PATH_MAX] = {"/usr", "/opt", "/var", "/home"};

  ct_mount_sort_candidates(candidates, 4U);
  if (strcmp(candidates[0], "/opt") != 0 || strcmp(candidates[1], "/usr") != 0 ||
      strcmp(candidates[2], "/var") != 0 || strcmp(candidates[3], "/home") != 0) {
    (void)fputs("mount candidate ordering is not length then lexical\n", stderr);
    return 1;
  }
  return 0;
}
