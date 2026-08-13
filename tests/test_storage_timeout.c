/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "storage_timeout.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int timeout_compound(void *context)
{
  (void)context;
  return 42;
}

int main(void)
{
  struct stat status;
  int descriptors[2];
  char byte;

  if (setenv("CT_RUNTIME_STORAGE_TIMEOUT", "zero", 1) != 0 ||
      ct_storage_timeout_is_valid()) {
    (void)fputs("malformed storage timeout was accepted\n", stderr);
    return 1;
  }
  if (setenv("CT_RUNTIME_STORAGE_TIMEOUT", "0", 1) != 0 ||
      ct_storage_timeout_is_valid()) {
    (void)fputs("zero storage timeout was accepted\n", stderr);
    return 1;
  }
  if (setenv("CT_RUNTIME_STORAGE_TIMEOUT", "3600.000001", 1) != 0 ||
      ct_storage_timeout_is_valid()) {
    (void)fputs("storage timeout above the native cap was accepted\n", stderr);
    return 1;
  }
  if (setenv("CT_RUNTIME_STORAGE_TIMEOUT", "0.1", 1) != 0 ||
      !ct_storage_timeout_is_valid() || pipe(descriptors) != 0 ||
      ct_storage_timeout_read(descriptors[0], &byte, 1U) >= 0 || errno != ETIMEDOUT) {
    (void)fputs("storage deadline did not interrupt a blocked operation\n", stderr);
    return 1;
  }
  (void)close(descriptors[0]);
  (void)close(descriptors[1]);
  if (setenv("CT_TEST_STORAGE_TIMEOUT_FORCE", "1", 1) != 0 ||
      ct_storage_timeout_lstat("/", &status) == 0 || errno != ETIMEDOUT) {
    (void)fputs("forced storage timeout was not reported\n", stderr);
    return 1;
  }
  if (ct_storage_timeout_call(timeout_compound, NULL) >= 0 ||
      errno != ETIMEDOUT) return 1;
  (void)unsetenv("CT_TEST_STORAGE_TIMEOUT_FORCE");
  if (ct_storage_timeout_call(timeout_compound, NULL) != 42) return 1;
  return 0;
}
