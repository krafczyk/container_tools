/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "package.h"

#include "package_identity.h"

#include <string.h>

#define CT_PACKAGE_FAILURE 78

static const char *const ct_compatibility_scripts[] = {
  "ct_exec.sh", "ct_shell.sh", "ct_instance_exec.sh", "ct_mount_detector.sh",
  "ct_args.sh",
};

const char *ct_package_version_json(void)
{
  return "{\"product_version\":\"" CT_PRODUCT_VERSION
         "\",\"source_commit\":\"" CT_SOURCE_COMMIT
         "\",\"architecture\":\"" CT_ARCHITECTURE
         "\",\"mount_plan_grammar\":\"" CT_MANIFEST_GRAMMAR "\"}";
}

int ct_package_verify_compatibility_identity(const char *identity,
                                              const char *script_name)
{
  size_t index;

  if (identity == NULL || script_name == NULL || strcmp(identity, CT_BUILD_IDENTITY) != 0) {
    return CT_PACKAGE_FAILURE;
  }
  for (index = 0U; index < sizeof(ct_compatibility_scripts) /
                                  sizeof(ct_compatibility_scripts[0]);
       ++index) {
    if (strcmp(script_name, ct_compatibility_scripts[index]) == 0) return 0;
  }
  return CT_PACKAGE_FAILURE;
}
