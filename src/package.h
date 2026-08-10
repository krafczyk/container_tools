/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_PACKAGE_H
#define CONTAINER_TOOLS_PACKAGE_H

/**
 * Return the compile-time identity object embedded in this executable.
 *
 * @return A NUL-terminated JSON object without a trailing newline.
 */
const char *ct_package_version_json(void);

/**
 * Check a generated compatibility-script identity before command dispatch.
 *
 * @param identity Build identity supplied by the sibling compatibility script.
 * @param script_name Expected installed compatibility-script basename.
 * @return Zero on a matching immutable identity, otherwise a failure status.
 */
int ct_package_verify_compatibility_identity(const char *identity,
                                             const char *script_name);

#endif
