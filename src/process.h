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
 * Runtime operations are `probe`, `create`, `start`, or `cleanup`; their
 * deadlines come from `CT_RUNTIME_<OPERATION>_TIMEOUT`, then
 * `CT_RUNTIME_OPERATION_TIMEOUT`, then ten seconds. Persistent operations are
 * `instance-probe` and `instance-start`; they use the established
 * `CT_INSTANCE_*_TIMEOUT` variables and five/30-second defaults. The runner
 * owns the child process group, forwards and then restores caller interruption,
 * escalates TERM to KILL, and returns 124 only for deadline expiry.
 */
int ct_process_run_operation(char *const arguments[],
                              const char *operation);

/** Run an operation while discarding its standard output and error. */
int ct_process_run_operation_quiet(char *const arguments[],
                                   const char *operation);

/** Run an operation with its standard output redirected to standard error. */
int ct_process_run_operation_stdout_to_stderr(char *const arguments[],
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

/** Capture bounded standard output while discarding operation diagnostics. */
int ct_process_run_operation_capture_quiet(char *const arguments[],
                                           const char *operation, char output[],
                                           size_t output_size);

#endif
