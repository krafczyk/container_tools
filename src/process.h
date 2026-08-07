/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_PROCESS_H
#define CONTAINER_TOOLS_PROCESS_H

#include <stddef.h>

/** One NUL-terminated environment assignment applied only in the child process. */
struct ct_process_environment { const char *name; const char *value; };

/**
 * Run one argv-preserved child in an owned process group and wait for its outcome.
 *
 * @param arguments Null-terminated executable argv.
 * @param environment Bounded child-only environment overrides.
 * @param environment_count Number of environment entries.
 * @return Exact normal child status, 128 plus its terminating signal, or 125
 *         when process setup, waiting, or signal-state restoration fails.
 */
int ct_process_run(char *const arguments[],
                   const struct ct_process_environment *environment,
                   size_t environment_count);

#endif
