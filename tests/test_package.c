/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "package.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect_architecture(const char *input, const char *expected)
{
  char output[CT_PACKAGE_ARCHITECTURE_MAX];
  if (!ct_package_normalize_architecture(input, output, sizeof(output)) ||
      strcmp(output, expected) != 0) {
    fprintf(stderr, "unexpected architecture result for %s\n", input);
    failures++;
  }
}

int main(void)
{
  const char *release_json = ct_package_release_json();
  char changed_json[CT_PACKAGE_RELEASE_JSON_MAX];

  if (!ct_package_release_json_is_current(release_json)) {
    fprintf(stderr, "generated release JSON was rejected\n");
    failures++;
  }
  if (strlen(release_json) >= sizeof(changed_json)) {
    fprintf(stderr, "generated release JSON exceeded its public bound\n");
    return 1;
  }
  strcpy(changed_json, release_json);
  changed_json[1] = changed_json[1] == 'x' ? 'y' : 'x';
  if (ct_package_release_json_is_current(changed_json)) {
    fprintf(stderr, "changed release JSON was accepted\n");
    failures++;
  }

  expect_architecture("x86_64", "x86_64");
  expect_architecture("aarch64", "aarch64");
  expect_architecture("ppc64le", "ppc64le");
  if (ct_package_normalize_architecture("bad/architecture", changed_json,
                                        sizeof(changed_json))) {
    fprintf(stderr, "unsafe architecture was accepted\n");
    failures++;
  }

  return failures == 0 ? 0 : 1;
}
