/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_RUNTIME_H
#define CONTAINER_TOOLS_RUNTIME_H

/**
 * Parse and run a native nonpersistent exec or shell launch.
 *
 * Singleton launcher options reject duplicate occurrences before runtime or
 * state access. Explicit binds accept writable `HOST:CONTAINER` and read-only
 * `HOST:CONTAINER:ro` values.
 *
 * @param argument_count Number of launcher arguments in `arguments`.
 * @param arguments Mutable launcher argument vector beginning with a backend.
 * @param shell_mode Nonzero to launch the configured interactive shell.
 * @return 64 for invalid arguments, the payload or backend status for an
 * attempted launch, or a nonzero internal failure status.
 */
int ct_runtime_foreground_command(int argument_count, char *const arguments[], int shell_mode);

#endif
