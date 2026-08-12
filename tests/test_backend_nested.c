/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_nested.h"
#include "executable.h"
#include "package_identity.h"
#include "trampoline.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int descriptor, const char *value)
{
  size_t used = 0U;
  const size_t length = strlen(value);
  while (used < length) {
    const ssize_t count = write(descriptor, value + used, length - used);
    if (count <= 0) return 1;
    used += (size_t)count;
  }
  return 0;
}

static int append_log(const char *backend, const char *kind)
{
  char line[64];
  const char *path = getenv("CT_NESTED_LOG");
  const int descriptor = path == NULL ? -1 : open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
  const int written = snprintf(line, sizeof(line), "%s %s\n", backend, kind);
  const int result = descriptor < 0 || written < 0 || (size_t)written >= sizeof(line) ||
                         write_all(descriptor, line) != 0;
  if (descriptor >= 0 && close(descriptor) != 0) return 1;
  return result;
}

static const char *base_name(const char *path)
{
  const char *separator = strrchr(path, '/');
  return separator == NULL ? path : separator + 1;
}

static int fake_backend_main(int argument_count, char **arguments)
{
  const char *backend = base_name(arguments[0]);
  const char *expected_pid = getenv("CT_NESTED_EXPECT_PID");
  const char *trampoline = NULL;
  const char *cwd = NULL;
  int probe = 0;
  int index;
  for (index = 1; index < argument_count; ++index) {
    if ((strcmp(arguments[index], "--chdir") == 0 || strcmp(arguments[index], "-w") == 0) &&
        index + 1 < argument_count) cwd = arguments[++index];
    else if (strcmp(arguments[index], "--probe") == 0) probe = 1;
    else if (strncmp(arguments[index], "/proc/self/fd/", 14U) == 0 &&
             index + 1 < argument_count &&
             strcmp(arguments[index + 1], "--internal-trampoline") == 0) {
      int remaining;
      trampoline = arguments[index];
      for (remaining = index + 1; remaining < argument_count; ++remaining) {
        if (strcmp(arguments[remaining], "--probe") == 0) probe = 1;
      }
      break;
    }
  }
  if (trampoline == NULL || cwd == NULL ||
      (probe == 0 &&
       (expected_pid == NULL || strtol(expected_pid, NULL, 10) != getpid())) ||
      append_log(backend, probe != 0 ? "probe" : "launch") != 0 ||
      chdir(cwd) != 0) return 125;
  if (probe != 0) {
    const char *configured = strcmp(backend, "bwrap") == 0
                                 ? getenv("CT_FAKE_BWRAP_PROBE")
                                 : getenv("CT_FAKE_PROOT_PROBE");
    if (configured != NULL && strcmp(configured, "0") != 0) return atoi(configured);
  }
  execv(trampoline, arguments + index + 1);
  return 126;
}

static int payload_main(int argument_count, char **arguments)
{
  const char *status;
  (void)arguments;
  if (argument_count != 2 || getenv("CT_NESTED_MUST_BE_ABSENT") != NULL ||
      fcntl(200, F_GETFD) < 0 || getenv("CT_NESTED_EXPECT_CWD") == NULL) return 125;
  status = getenv("CT_NESTED_PAYLOAD_RESULT");
  if (status != NULL && strcmp(status, "signal") == 0) raise(SIGTERM);
  return status == NULL ? 0 : atoi(status);
}

static int line_count(const char *path)
{
  FILE *stream = fopen(path, "r");
  int lines = 0;
  int byte;
  if (stream == NULL) return -1;
  while ((byte = fgetc(stream)) != EOF) if (byte == '\n') ++lines;
  return fclose(stream) != 0 ? -1 : lines;
}

static int clear_log(const char *path)
{
  const int descriptor = open(path, O_WRONLY | O_TRUNC | O_CREAT, 0600);
  return descriptor < 0 || close(descriptor) != 0;
}

static int ct_test_nested_execute(const struct ct_nested_request *request,
                                  const char *forced, int allow_rewrite)
{
  const pid_t child = fork();
  int status;
  if (child < 0) return 125;
  if (child == 0) {
    char expected[32];
    if (snprintf(expected, sizeof(expected), "%lu", (unsigned long)getpid()) >=
            (int)sizeof(expected) ||
        setenv("CT_NESTED_EXPECT_PID", expected, 1) != 0) _exit(125);
    _exit(ct_backend_nested_execute(request, forced, allow_rewrite));
  }
  if (waitpid(child, &status, 0) != child) return 125;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
}

int main(int argument_count, char **arguments)
{
  char work[] = "/tmp/mkchad-v1/container-tools-c11/nested.XXXXXX";
  char root[4096], proc[4096], bwrap[4096], proot[4096], log[4096], self[64];
  char *payload[] = {self, "--nested-payload", NULL};
  int trampoline, control, inherited, temporary, bad;
  struct ct_host_profile profile;
  struct ct_path_map map;
  struct ct_nested_request request;
  struct ct_process_environment environment[7];
  struct ct_nested_backend_report reports[3];
  enum ct_nested_pre_dispatch_failure failure;
  if (strcmp(base_name(arguments[0]), "bwrap") == 0 || strcmp(base_name(arguments[0]), "proot") == 0) {
    return fake_backend_main(argument_count, arguments);
  }
  if (strcmp(arguments[0], "--internal-trampoline") == 0) {
    return ct_trampoline_main(argument_count, arguments, CT_BUILD_IDENTITY);
  }
  if (argument_count >= 2 && strcmp(arguments[1], "--internal-trampoline") == 0) {
    return ct_trampoline_main(argument_count - 1, arguments + 1, CT_BUILD_IDENTITY);
  }
  if (argument_count == 2 && strcmp(arguments[1], "--nested-payload") == 0) {
    return payload_main(argument_count, arguments);
  }
  if (snprintf(self, sizeof(self), "/proc/self/exe") >= (int)sizeof(self) ||
      mkdtemp(work) == NULL || snprintf(root, sizeof(root), "%s/root", work) >= (int)sizeof(root) ||
      snprintf(proc, sizeof(proc), "%s/proc", root) >= (int)sizeof(proc) ||
      snprintf(bwrap, sizeof(bwrap), "%s/bwrap", work) >= (int)sizeof(bwrap) ||
      snprintf(proot, sizeof(proot), "%s/proot", work) >= (int)sizeof(proot) ||
      snprintf(log, sizeof(log), "%s/log", work) >= (int)sizeof(log) ||
      mkdir(root, 0700) != 0 || mkdir(proc, 0700) != 0 ||
      symlink("/proc/self/exe", bwrap) != 0 || symlink("/proc/self/exe", proot) != 0) return 1;
  memset(&profile, 0, sizeof(profile));
  strcpy(profile.root, "/"); strcpy(profile.root_access, "inherit");
  strcpy(profile.semantics, "full-root");
  ct_path_map_init(&map);
  if (ct_path_map_set_root(&map, "/") != 0 || setenv("CT_NESTED_MUST_BE_ABSENT", "secret", 1) != 0) return 2;
  environment[0] = (struct ct_process_environment){"CT_NESTED_LOG", log};
  environment[1] = (struct ct_process_environment){"CT_NESTED_EXPECT_CWD", "/"};
  environment[2] = (struct ct_process_environment){"CT_NESTED_PAYLOAD_RESULT", "42"};
  environment[3] = (struct ct_process_environment){"CT_FAKE_BWRAP_PROBE", "0"};
  environment[4] = (struct ct_process_environment){"CT_FAKE_PROOT_PROBE", "0"};
  environment[5] = (struct ct_process_environment){"CT_NESTED_MUST_BE_ABSENT", NULL};
  environment[6] = (struct ct_process_environment){"PATH", "/not-a-backend-path"};
  temporary = open("/dev/null", O_RDONLY);
  trampoline = open("/proc/self/exe", O_RDONLY);
  control = ct_executable_control_open(CT_BUILD_IDENTITY);
  inherited = temporary < 0 ? -1 : dup2(temporary, 200);
  if (temporary < 0 || trampoline < 0 || control < 0 || inherited != 200 ||
      close(temporary) != 0) return 3;
  memset(&request, 0, sizeof(request));
  request.profile = &profile; request.map = &map; request.cwd = "/";
  request.payload = payload; request.environment = environment;
  request.environment_count = sizeof(environment) / sizeof(environment[0]);
  request.trampoline_descriptor = trampoline; request.control_descriptor = control;
  request.inherited_descriptor = inherited; request.bubblewrap_path = bwrap;
  request.proot_path = proot;
  if (ct_test_nested_execute(&request, "bubblewrap", 0) != 42 || line_count(log) != 2) return 4;
  if (clear_log(log) != 0) return 5;
  environment[2].value = "signal";
  if (ct_test_nested_execute(&request, "bubblewrap", 0) != 128 + SIGTERM ||
      line_count(log) != 2) return 5;
  environment[2].value = "41";
  environment[3].value = "7";
  if (clear_log(log) != 0 || ct_test_nested_execute(&request, NULL, 0) != 41 ||
      line_count(log) != 3) return 6;
  environment[3].value = "0";
  if (clear_log(log) != 0 || ct_backend_nested_diagnose(&request, 0, reports) != 0 ||
      reports[0].operational != CT_NESTED_OPERATIONAL_YES ||
      strcmp(reports[1].reason_code, "earlier-backend-ready") != 0 ||
       line_count(log) != 1) return 7;
  environment[3].value = "126";
  if (ct_backend_nested_execute_detailed(&request, "bubblewrap", 0, &failure) !=
          125 ||
      failure != CT_NESTED_PRE_DISPATCH_POLICY_DENIED) return 7;
  environment[3].value = "124";
  if (ct_backend_nested_execute_detailed(&request, "bubblewrap", 0, &failure) !=
          125 ||
      failure != CT_NESTED_PRE_DISPATCH_PROBE_TIMEOUT) return 7;
  environment[3].value = "0";
  request.bubblewrap_path = "/not-a-container-tools-backend";
  if (ct_backend_nested_execute_detailed(&request, "bubblewrap", 0, &failure) !=
          125 ||
      failure != CT_NESTED_PRE_DISPATCH_TOOL_MISSING) return 7;
  request.bubblewrap_path = bwrap;
  bad = open("/dev/null", O_RDONLY);
  if (bad < 0) return 8;
  request.trampoline_descriptor = bad;
  if (clear_log(log) != 0 ||
      ct_backend_nested_execute_detailed(&request, "bubblewrap", 0, &failure) !=
          125 ||
      failure != CT_NESTED_PRE_DISPATCH_TRAMPOLINE || line_count(log) != 0 ||
      close(bad) != 0) return 8;
  request.trampoline_descriptor = trampoline;
#ifndef CT_TRAMPOLINE_TEST_SEAM
  bad = open("/dev/null", O_RDONLY);
  if (bad < 0) return 9;
  request.control_descriptor = bad;
  if (clear_log(log) != 0 || ct_test_nested_execute(&request, "bubblewrap", 0) != 125 ||
      line_count(log) != 1 || close(bad) != 0) return 9;
  request.control_descriptor = control;
#endif
  strcpy(profile.root_access, "read-only");
  if (ct_backend_nested_requires_read_only(&request) == 0 ||
      ct_test_nested_execute(&request, "proot", 0) != 125) return 10;
  ct_path_map_destroy(&map);
  return close(inherited) != 0 || close(control) != 0 || close(trampoline) != 0 ||
                 unlink(log) != 0 || unlink(bwrap) != 0 || unlink(proot) != 0 ||
                 rmdir(proc) != 0 || rmdir(root) != 0 || rmdir(work) != 0
             ? 11
             : 0;
}
