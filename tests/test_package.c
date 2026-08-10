/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "package.h"

#include <stdio.h>
#include <string.h>

static int failures;

int main(void)
{
  const char *version_json = ct_package_version_json();

  if (strstr(version_json, "product_version") == NULL ||
      strstr(version_json, "source_commit") == NULL ||
      strstr(version_json, "architecture") == NULL ||
      strstr(version_json, "mount_plan_grammar") == NULL) {
    fprintf(stderr, "version JSON omitted compile-time identity\n");
    failures++;
  }

  return failures == 0 ? 0 : 1;
}
