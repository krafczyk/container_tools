/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_PACKAGE_H
#define CONTAINER_TOOLS_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define CT_PACKAGE_ARCHITECTURE_MAX 128U
#define CT_PACKAGE_RELEASE_JSON_MAX 2048U

/**
 * Return the immutable closed release object embedded in this executable.
 *
 * @return A NUL-terminated JSON object without a trailing newline.
 */
const char *ct_package_release_json(void);

/**
 * Compare a JSON object with the complete embedded release schema.
 *
 * @param release_json Candidate NUL-terminated JSON bytes.
 * @return True only when the bytes exactly match the closed schema.
 */
bool ct_package_release_json_is_current(const char *release_json);

/**
 * Normalize an arbitrary kernel architecture token without an allowlist.
 *
 * @param input NUL-terminated architecture token.
 * @param output Destination for ASCII-lowercase output.
 * @param output_size Size of output in bytes.
 * @return True for a bounded portable token, false for malformed input.
 */
bool ct_package_normalize_architecture(const char *input, char *output,
                                       size_t output_size);

/**
 * Verify the executable, metadata, and compatibility scripts in this prefix.
 *
 * @param diagnostics Stream for bounded failure or success diagnostics; may be NULL.
 * @param json Emit the closed release object on successful verification.
 * @return Zero when the complete package is coherent, otherwise a failure status.
 */
int ct_package_verify(FILE *diagnostics, bool json);

/**
 * Validate package coherence without emitting a success result.
 *
 * @param diagnostics Stream for a failure diagnostic; may be NULL.
 * @return Zero when the complete package is coherent, otherwise a failure status.
 */
int ct_package_validate(FILE *diagnostics);

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
