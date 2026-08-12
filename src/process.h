/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_PROCESS_H
#define CONTAINER_TOOLS_PROCESS_H

#include <stddef.h>

#define CT_PROCESS_ENVIRONMENT_LIMIT 129U

/** One environment operation; a NULL value removes the variable. */
struct ct_process_environment { const char *name; const char *value; };

/**
 * Apply bounded environment removals and overrides to the current process.
 *
 * Operations are applied in order and are not rolled back if a later operation
 * fails.
 *
 * @param environment Environment removals and overrides.
 * @param environment_count Number of environment entries.
 * @return Zero on success or one for invalid input or an environment failure.
 */
int ct_process_apply_environment(
    const struct ct_process_environment *environment,
    size_t environment_count);

/**
 * Apply a bounded environment plan and replace this process with `arguments`.
 *
 * Standard descriptors, process group, terminal ownership, signals, and shell
 * job control pass directly to the selected executable. This function returns
 * only when setup or exec fails. Applied environment changes remain in the
 * current process if exec fails.
 *
 * @param arguments Null-terminated executable argv.
 * @param environment Bounded environment removals and overrides.
 * @param environment_count Number of environment entries.
 * @return 64 for invalid input, 125 for environment setup failure, 127 when
 *         the executable is absent, or 126 for another exec failure.
 */
int ct_process_exec(char *const arguments[],
                    const struct ct_process_environment *environment,
                    size_t environment_count);

/**
 * Run one argv-preserved child and wait for its outcome.
 *
 * The child runs in an owned process group. When the caller owns an inherited
 * foreground terminal, that terminal is handed to the child group for the
 * duration of the command and restored before return. Child stops suspend the
 * caller job and preserve foreground continuation. Signals delivered only to
 * the supervisor are forwarded to the complete child group.
 *
 * @param arguments Null-terminated executable argv.
 * @param environment Bounded child-only environment removals and overrides.
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
