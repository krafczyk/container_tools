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

/**
 * Run an external control operation under its named monotonic deadline.
 *
 * The operation must be `probe`, `create`, `start`, or `cleanup`.  Its
 * deadline comes from `CT_RUNTIME_<OPERATION>_TIMEOUT`, falling back to
 * `CT_RUNTIME_OPERATION_TIMEOUT` and then ten seconds.  The runner owns the
 * child process group, forwards interruption, escalates TERM to KILL, and
 * returns 124 for a deadline expiry or interrupted cleanup failure.
 */
int ct_process_run_operation(char *const arguments[],
                              const char *operation);

/**
 * Run an operation and copy its bounded standard output into `output`.
 *
 * `output` must provide at least two bytes. The function always NUL terminates
 * successful capture, fails with 125 when output exceeds the supplied buffer,
 * and otherwise returns the operation result described above.
 */
int ct_process_run_operation_capture(char *const arguments[], const char *operation,
                                     char output[], size_t output_size);

#endif
