/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "storage.h"

#include <stdio.h>

int main(void)
{
  if (!ct_storage_architecture_is_safe("x86_64") ||
      ct_storage_architecture_is_safe("../outside") ||
      ct_storage_architecture_is_safe(".")) {
    (void)fputs("architecture validation did not preserve the Bash contract\n", stderr);
    return 1;
  }
  if (ct_storage_ensure_private_directory("/tmp/container-tools-storage/..") == 0 ||
      ct_storage_ensure_private_directory("/tmp/container-tools-storage/.") == 0) {
    (void)fputs("terminal dot path component was accepted\n", stderr);
    return 1;
  }
  return 0;
}
