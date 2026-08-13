/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_CLI_H
#define CONTAINER_TOOLS_CLI_H

#include <stdio.h>

/** Closed native commands accepted by the container-tools bootstrap parser. */
enum ct_command {
  CT_COMMAND_HELP,
  CT_COMMAND_VERSION,
  CT_COMMAND_EXEC,
  CT_COMMAND_SHELL,
  CT_COMMAND_INSTANCE_EXEC,
  CT_COMMAND_INSTANCE_IDENTITY,
  CT_COMMAND_INSTANCE_INSPECT,
  CT_COMMAND_INSTANCE_PROFILE_INSPECT,
  CT_COMMAND_MOUNT_DETECT,
  CT_COMMAND_MOUNT_ARGS,
  CT_COMMAND_MOUNT_PLAN_INSPECT,
  CT_COMMAND_MOUNT_PLAN_COMPARE,
  CT_COMMAND_MOUNT_PLAN_CLEAR,
  CT_COMMAND_MOUNT_PLAN_LOCATION,
  CT_COMMAND_HOST_EXEC,
  CT_COMMAND_HOST_DOCTOR,
};

/** Parse status values returned before any package or backend access. */
enum ct_cli_status {
  CT_CLI_OK = 0,
  CT_CLI_INVALID = 1,
};

/** Parsed bootstrap command and its JSON-output request. */
struct ct_cli {
  enum ct_command command;
  int json;
};

/**
 * Parse one native CLI invocation without accessing package state.
 *
 * @param argument_count Number of arguments after the executable name.
 * @param arguments Raw Linux argv byte strings after the executable name.
 * @param parsed Destination for the closed command result.
 * @return CT_CLI_OK on a supported grammar, otherwise CT_CLI_INVALID.
 */
enum ct_cli_status ct_cli_parse(int argument_count, const char *const *arguments,
                                struct ct_cli *parsed);

/** Print the stable top-level usage diagnostic to the supplied stream. */
void ct_cli_write_usage(FILE *stream);

/**
 * Write one bounded, human-facing failure diagnostic to standard error.
 *
 * @param operation Stable operation name without caller-supplied values.
 * @param category Stable cause category without caller-supplied values.
 * @param action Concrete corrective action without caller-supplied values.
 * @sideeffect Writes only to stderr; it never changes machine stdout.
 */
void ct_cli_diagnostic(const char *operation, const char *category,
                       const char *action);

#endif
