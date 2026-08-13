/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_BACKEND_BUBBLEWRAP_H
#define CONTAINER_TOOLS_BACKEND_BUBBLEWRAP_H

#include "backend_nested.h"

/**
 * Build one non-mutating Bubblewrap probe or launch over a private root.
 *
 * @param request Validated nested-execution inputs and selected path map.
 * @param probe Nonzero to append a semantic probe instead of the payload.
 * @param command Initialized command receiving the generated arguments.
 * @return A typed command-build result distinguishing unsupported roots from
 *         internal failure.
 */
enum ct_nested_build_result ct_backend_bubblewrap_arguments(
    const struct ct_nested_request *request, int probe,
    struct ct_nested_command *command);
#endif
