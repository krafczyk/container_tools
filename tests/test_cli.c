/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "cli.h"

#include <stdio.h>

static int failures;

static void expect_command(const char *const *arguments, int argument_count,
                           enum ct_command expected)
{
  struct ct_cli parsed;
  if (ct_cli_parse(argument_count, arguments, &parsed) != CT_CLI_OK ||
      parsed.command != expected) {
    fprintf(stderr, "unexpected parser result for %s\n", arguments[0]);
    failures++;
  }
}

static void expect_invalid(const char *const *arguments, int argument_count)
{
  struct ct_cli parsed;
  if (ct_cli_parse(argument_count, arguments, &parsed) == CT_CLI_OK) {
    fprintf(stderr, "accepted invalid parser shape for %s\n", arguments[0]);
    failures++;
  }
}

int main(void)
{
  const char *const exec_arguments[] = {"exec", "--", "image", "command"};
  const char *const shell_arguments[] = {"shell", "--", "image"};
  const char *const instance_exec_arguments[] = {"instance", "exec", "--", "image", "command"};
  const char *const instance_identity_arguments[] = {"instance", "identity"};
  const char *const mount_detect_arguments[] = {"mount", "detect"};
  const char *const mount_args_arguments[] = {"mount", "args", "path with spaces"};
  const char *const mount_plan_inspect_arguments[] = {"mount", "plan", "inspect", "--json"};
  const char *const mount_plan_compare_arguments[] = {"mount", "plan", "compare", "left"};
  const char *const mount_plan_clear_arguments[] = {"mount", "plan", "clear"};
  const char *const mount_plan_location_arguments[] = {"mount", "plan", "location"};
  const char *const host_exec_arguments[] = {"host", "exec", "--", "command"};
  const char *const host_doctor_arguments[] = {"host", "doctor", "--json"};
  const char *const invalid_subcommand[] = {"instance", "destroy"};
  const char *const invalid_option[] = {"--unknown"};
  const char *const duplicate_version[] = {"--version", "--version"};
  const char *const removed_runtime_exec[] = {"runtime", "exec", "--backend", "docker", "--", "docker", "run"};
  const char build_command[] = {'b', 'u', 'i', 'l', 'd', 'x', '\0'};
  const char *const removed_build_exec[] = {build_command, "exec", "--architecture", "x86_64", "--", "docker", build_command, "build"};
  const char *const removed_package_verify[] = {"package", "verify", "--json"};
  const char non_utf8_argument[] = {'e', 'x', 'e', 'c', '\0'};
  const char *const empty_argument[] = {"exec", "--", "image", ""};
  const char *const non_utf8_arguments[] = {non_utf8_argument, "--", "\xff"};

  expect_command(exec_arguments, 4, CT_COMMAND_EXEC);
  expect_command(shell_arguments, 3, CT_COMMAND_SHELL);
  expect_command(instance_exec_arguments, 5, CT_COMMAND_INSTANCE_EXEC);
  expect_command(instance_identity_arguments, 2, CT_COMMAND_INSTANCE_IDENTITY);
  expect_command(mount_detect_arguments, 2, CT_COMMAND_MOUNT_DETECT);
  expect_command(mount_args_arguments, 3, CT_COMMAND_MOUNT_ARGS);
  expect_command(mount_plan_inspect_arguments, 4, CT_COMMAND_MOUNT_PLAN_INSPECT);
  expect_command(mount_plan_compare_arguments, 4, CT_COMMAND_MOUNT_PLAN_COMPARE);
  expect_command(mount_plan_clear_arguments, 3, CT_COMMAND_MOUNT_PLAN_CLEAR);
  expect_command(mount_plan_location_arguments, 3, CT_COMMAND_MOUNT_PLAN_LOCATION);
  expect_command(host_exec_arguments, 4, CT_COMMAND_HOST_EXEC);
  expect_command(host_doctor_arguments, 3, CT_COMMAND_HOST_DOCTOR);
  expect_command(non_utf8_arguments, 3, CT_COMMAND_EXEC);
  expect_invalid(invalid_subcommand, 2);
  expect_invalid(invalid_option, 1);
  expect_invalid(duplicate_version, 2);
  expect_invalid(removed_runtime_exec, 7);
  expect_invalid(removed_build_exec, 8);
  expect_invalid(removed_package_verify, 3);
  expect_command(empty_argument, 4, CT_COMMAND_EXEC);

  return failures == 0 ? 0 : 1;
}
