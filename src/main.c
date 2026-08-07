/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "cli.h"
#include "package.h"

#include "package_identity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CT_EXIT_USAGE 64
#define CT_EXIT_NOT_IMPLEMENTED 70
#define CT_EXIT_PACKAGE 78

static const char *ct_command_name(enum ct_command command)
{
  switch (command) {
  case CT_COMMAND_EXEC: return "exec";
  case CT_COMMAND_SHELL: return "shell";
  case CT_COMMAND_INSTANCE_EXEC: return "instance exec";
  case CT_COMMAND_INSTANCE_IDENTITY: return "instance identity";
  case CT_COMMAND_MOUNT_DETECT: return "mount detect";
  case CT_COMMAND_MOUNT_ARGS: return "mount args";
  case CT_COMMAND_RUNTIME_EXEC: return "runtime exec";
  case CT_COMMAND_BUILDX_EXEC: return "buildx exec";
  case CT_COMMAND_HOST_EXEC: return "host exec";
  case CT_COMMAND_HOST_DOCTOR: return "host doctor";
  case CT_COMMAND_HELP:
  case CT_COMMAND_VERSION:
  case CT_COMMAND_PACKAGE_VERIFY: return "package verify";
  }
  return "unknown";
}

static int ct_internal_compatibility(int argument_count, char **arguments)
{
  if (argument_count < 4 ||
      ct_package_verify_compatibility_identity(arguments[2], arguments[3]) != 0) {
    (void)fputs("container-tools: package verification failed: compatibility identity mismatch\n",
                stderr);
    return CT_EXIT_PACKAGE;
  }
  if (ct_package_validate(stderr) != 0) {
    return CT_EXIT_PACKAGE;
  }
  (void)fprintf(stderr, "container-tools: not implemented: compat %s\n", arguments[3]);
  return CT_EXIT_NOT_IMPLEMENTED;
}

int main(int argument_count, char **arguments)
{
  struct ct_cli parsed;

  if (argument_count >= 2 && strcmp(arguments[1], "--internal-compat") == 0) {
    return ct_internal_compatibility(argument_count, arguments);
  }
  if (ct_cli_parse(argument_count - 1, (const char *const *)(arguments + 1), &parsed) !=
      CT_CLI_OK) {
    ct_cli_write_usage(stderr);
    return CT_EXIT_USAGE;
  }
  if (parsed.command == CT_COMMAND_PACKAGE_VERIFY) {
    return ct_package_verify(stdout, parsed.json != 0);
  }
  if (ct_package_validate(stderr) != 0) {
    return CT_EXIT_PACKAGE;
  }
  if (parsed.command == CT_COMMAND_HELP) {
    ct_cli_write_usage(stdout);
    return 0;
  }
  if (parsed.command == CT_COMMAND_VERSION) {
    if (parsed.json != 0) {
      (void)fprintf(stdout, "%s\n", ct_package_release_json());
    } else {
      (void)fprintf(stdout, "container-tools %s (%s)\n", CT_PRODUCT_VERSION,
                    CT_BUILD_IDENTITY);
    }
    return 0;
  }
  (void)fprintf(stderr, "container-tools: not implemented: %s\n",
                ct_command_name(parsed.command));
  return CT_EXIT_NOT_IMPLEMENTED;
}
