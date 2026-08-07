/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_RUNTIME_H
#define CONTAINER_TOOLS_RUNTIME_H

/** Parse and run a native nonpersistent exec or shell launch. */
int ct_runtime_foreground_command(int argument_count, char *const arguments[], int shell_mode);

#endif
