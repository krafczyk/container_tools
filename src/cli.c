/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "cli.h"

#include <stdbool.h>
#include <string.h>

static bool ct_arguments_are_present(int argument_count,
                                     const char *const *arguments)
{
  int index;

  if (argument_count < 1 || arguments == NULL) {
    return false;
  }
  for (index = 0; index < argument_count; ++index) {
    if (arguments[index] == NULL) {
      return false;
    }
  }
  return true;
}

static enum ct_cli_status ct_parse_pair(const char *first, const char *second,
                                        enum ct_command command,
                                        int argument_count,
                                        const char *const *arguments,
                                        struct ct_cli *parsed)
{
  if (argument_count < 2 || strcmp(arguments[0], first) != 0 ||
      strcmp(arguments[1], second) != 0) {
    return CT_CLI_INVALID;
  }
  parsed->command = command;
  return CT_CLI_OK;
}

enum ct_cli_status ct_cli_parse(int argument_count, const char *const *arguments,
                                struct ct_cli *parsed)
{
  if (parsed == NULL || !ct_arguments_are_present(argument_count, arguments) ||
      arguments[0][0] == '\0') {
    return CT_CLI_INVALID;
  }
  parsed->json = 0;
  if (argument_count == 1 &&
      (strcmp(arguments[0], "--help") == 0 || strcmp(arguments[0], "help") == 0)) {
    parsed->command = CT_COMMAND_HELP;
    return CT_CLI_OK;
  }
  if (strcmp(arguments[0], "--version") == 0) {
    if (argument_count == 1) {
      parsed->command = CT_COMMAND_VERSION;
      return CT_CLI_OK;
    }
    if (argument_count == 2 && strcmp(arguments[1], "--json") == 0) {
      parsed->command = CT_COMMAND_VERSION;
      parsed->json = 1;
      return CT_CLI_OK;
    }
    return CT_CLI_INVALID;
  }
  if (strcmp(arguments[0], "exec") == 0) {
    parsed->command = CT_COMMAND_EXEC;
    return CT_CLI_OK;
  }
  if (strcmp(arguments[0], "shell") == 0) {
    parsed->command = CT_COMMAND_SHELL;
    return CT_CLI_OK;
  }
  if (strcmp(arguments[0], "instance") == 0) {
    if (ct_parse_pair("instance", "exec", CT_COMMAND_INSTANCE_EXEC, argument_count,
                      arguments, parsed) == CT_CLI_OK ||
        ct_parse_pair("instance", "identity", CT_COMMAND_INSTANCE_IDENTITY,
                      argument_count, arguments, parsed) == CT_CLI_OK) {
      return CT_CLI_OK;
    }
    return CT_CLI_INVALID;
  }
  if (strcmp(arguments[0], "mount") == 0) {
    if (ct_parse_pair("mount", "detect", CT_COMMAND_MOUNT_DETECT, argument_count,
                      arguments, parsed) == CT_CLI_OK ||
        ct_parse_pair("mount", "args", CT_COMMAND_MOUNT_ARGS, argument_count,
                      arguments, parsed) == CT_CLI_OK) {
      return CT_CLI_OK;
    }
    return CT_CLI_INVALID;
  }
  if (strcmp(arguments[0], "host") == 0) {
    if (ct_parse_pair("host", "exec", CT_COMMAND_HOST_EXEC, argument_count,
                      arguments, parsed) == CT_CLI_OK ||
        ct_parse_pair("host", "doctor", CT_COMMAND_HOST_DOCTOR, argument_count,
                      arguments, parsed) == CT_CLI_OK) {
      return CT_CLI_OK;
    }
    return CT_CLI_INVALID;
  }
  return CT_CLI_INVALID;
}

void ct_cli_write_usage(FILE *stream)
{
  (void)fputs("usage: container-tools [--help] [--version [--json]] COMMAND ...\n"
               "commands: exec, shell, instance exec|identity, mount detect|args,\n"
               "          host exec|doctor\n",
               stream);
}
