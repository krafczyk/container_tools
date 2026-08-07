/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "buildx.h"

#include <stdio.h>

int main(void)
{
  const char *valid[] = {"docker", "buildx", "build", "--tag", "image"};
  const char *invalid[] = {"docker", "buildx", "build", "--cache-to", "type=local"};

  if (!ct_buildx_child_is_valid(valid, 5U) || ct_buildx_child_is_valid(invalid, 5U)) {
    (void)fputs("Buildx child validation changed\n", stderr);
    return 1;
  }
  return 0;
}
