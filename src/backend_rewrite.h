/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_BACKEND_REWRITE_H
#define CONTAINER_TOOLS_BACKEND_REWRITE_H
#include "backend_nested.h"
#include <stdio.h>
/**
 * Build direct native static ELF or bounded shebang-entry weak execution.
 *
 * Every retained execution-stage descriptor and a dynamic PT_INTERP loader are
 * checked for set-id bits and file capabilities before command construction.
 *
 * @param request Prepared selected-root request with a resolved executable.
 * @param command Empty caller-owned command to populate.
 * @return Zero after a NULL-terminated direct argv is built, otherwise nonzero
 *         without admitting a rewrite launch.
 */
int ct_backend_rewrite_arguments(const struct ct_nested_request *request,
                                  struct ct_nested_command *command);
/**
 * Write the complete warning emitted before weak rewrite dispatch.
 *
 * @param stream Destination diagnostic stream.
 * @param requested_semantics Profile semantics requested by the caller.
 * @param outcomes Fixed-order selection outcomes for Bubblewrap and PRoot.
 * @return Zero after the complete warning, otherwise nonzero.
 */
int ct_backend_rewrite_warning(
    FILE *stream, const char *requested_semantics,
    const struct ct_nested_backend_report outcomes[CT_NESTED_BACKEND_COUNT]);
#endif
