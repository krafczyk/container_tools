/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_nested.h"

#include "backend_bubblewrap.h"
#include "backend_proot.h"
#include "backend_rewrite.h"
#include "supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *ct_backend_nested_name(enum ct_nested_backend backend)
{
  static const char *const names[] = {"bubblewrap", "proot", "rewrite"};
  return backend >= CT_NESTED_BACKEND_BUBBLEWRAP &&
                 backend <= CT_NESTED_BACKEND_REWRITE
             ? names[(size_t)backend]
             : NULL;
}

int ct_nested_command_init(struct ct_nested_command *command, size_t capacity)
{
  if (command == NULL || capacity < 2U || capacity > CT_NESTED_ARGUMENT_LIMIT) {
    return 1;
  }
  memset(command, 0, sizeof(*command));
  command->arguments = calloc(capacity, sizeof(*command->arguments));
  command->owned = calloc(capacity, sizeof(*command->owned));
  if (command->arguments == NULL || command->owned == NULL) {
    ct_nested_command_destroy(command);
    return 1;
  }
  command->capacity = capacity;
  return 0;
}

int ct_nested_command_add(struct ct_nested_command *command, const char *value)
{
  if (command == NULL || value == NULL || command->arguments == NULL ||
      command->count + 1U >= command->capacity) {
    return 1;
  }
  command->arguments[command->count++] = (char *)value;
  return 0;
}

static int ct_nested_command_own(struct ct_nested_command *command, char *value)
{
  if (value == NULL || command == NULL || command->owned == NULL ||
      command->owned_count >= command->capacity ||
      ct_nested_command_add(command, value) != 0) {
    free(value);
    return 1;
  }
  command->owned[command->owned_count++] = value;
  return 0;
}

int ct_nested_command_add_pair(struct ct_nested_command *command,
                               const char *first, const char *separator,
                               const char *second)
{
  size_t first_length;
  size_t separator_length;
  size_t second_length;
  char *value;
  if (first == NULL || separator == NULL || second == NULL) {
    return 1;
  }
  first_length = strlen(first);
  separator_length = strlen(separator);
  second_length = strlen(second);
  if (first_length > CT_HOST_PATH_MAX || second_length > CT_HOST_PATH_MAX ||
      separator_length > 1U || first_length + separator_length + second_length <
                                  first_length ||
      first_length + separator_length + second_length >=
          CT_HOST_PATH_MAX * 2U + 2U) {
    return 1;
  }
  value = malloc(first_length + separator_length + second_length + 1U);
  if (value == NULL) {
    free(value);
    return 1;
  }
  memcpy(value, first, first_length);
  memcpy(value + first_length, separator, separator_length);
  memcpy(value + first_length + separator_length, second, second_length + 1U);
  return ct_nested_command_own(command, value);
}

int ct_nested_command_add_descriptor(struct ct_nested_command *command,
                                      const char *prefix, int descriptor)
{
  int needed;
  char *value;
  if (prefix == NULL || strlen(prefix) > 32U || descriptor < 0 ||
      (needed = snprintf(NULL, 0, "%s%d", prefix, descriptor)) < 0 ||
      needed > 63) {
    return 1;
  }
  value = malloc((size_t)needed + 1U);
  if (value == NULL ||
      snprintf(value, (size_t)needed + 1U, "%s%d", prefix, descriptor) != needed) {
    free(value);
    return 1;
  }
  return ct_nested_command_own(command, value);
}

static int ct_nested_command_add_uintmax(struct ct_nested_command *command,
                                         uintmax_t value)
{
  const int needed = snprintf(NULL, 0, "%ju", value);
  char *text;
  if (needed < 0 || needed > 63) return 1;
  text = malloc((size_t)needed + 1U);
  if (text == NULL || snprintf(text, (size_t)needed + 1U, "%ju", value) != needed) {
    free(text);
    return 1;
  }
  return ct_nested_command_own(command, text);
}

static int ct_nested_command_add_probe(const struct ct_nested_request *request,
                                       struct ct_nested_command *command)
{
  const char *source;
  const char *target;
  struct stat metadata;
  if (request == NULL || request->profile == NULL || request->map == NULL) return 1;
  if (request->map->count == 0U) {
    source = request->profile->root;
    target = "/";
  } else {
    source = request->map->entries[0].visible;
    target = request->map->entries[0].target;
  }
  if (stat(source, &metadata) != 0 ||
      ct_nested_command_add(command, "--probe") != 0 ||
      ct_nested_command_add(command, request->cwd) != 0 ||
      ct_nested_command_add(command, target) != 0 ||
      ct_nested_command_add_uintmax(command, (uintmax_t)metadata.st_dev) != 0 ||
      ct_nested_command_add_uintmax(command, (uintmax_t)metadata.st_ino) != 0 ||
      ct_nested_command_add_uintmax(command,
                                    (uintmax_t)(metadata.st_mode & S_IFMT)) != 0) {
    return 1;
  }
  return 0;
}

int ct_nested_command_finish(struct ct_nested_command *command)
{
  if (command == NULL || command->arguments == NULL || command->count == 0U ||
      command->count >= command->capacity) {
    return 1;
  }
  command->arguments[command->count] = NULL;
  return 0;
}

void ct_nested_command_destroy(struct ct_nested_command *command)
{
  size_t index;
  if (command == NULL) return;
  for (index = 0U; index < command->owned_count; ++index) {
    free(command->owned[index]);
  }
  free(command->owned);
  free(command->arguments);
  memset(command, 0, sizeof(*command));
}

int ct_nested_command_add_trampoline(const struct ct_nested_request *request,
                                     int probe,
                                     struct ct_nested_command *command)
{
  size_t index;
  if (request == NULL || command == NULL || request->trampoline_descriptor < 0 ||
       request->control_descriptor < 0 ||
      fcntl(request->trampoline_descriptor, F_GETFD) < 0 ||
      fcntl(request->control_descriptor, F_GETFD) < 0 ||
      (fcntl(request->trampoline_descriptor, F_GETFD) & FD_CLOEXEC) != 0 ||
      (fcntl(request->control_descriptor, F_GETFD) & FD_CLOEXEC) != 0 ||
      (request->inherited_descriptor >= 3 &&
       (fcntl(request->inherited_descriptor, F_GETFD) < 0 ||
        (fcntl(request->inherited_descriptor, F_GETFD) & FD_CLOEXEC) != 0)) ||
      ct_nested_command_add_descriptor(command, "/proc/self/fd/",
                                       request->trampoline_descriptor) != 0 ||
      ct_nested_command_add(command, "--internal-trampoline") != 0 ||
      ct_nested_command_add(command, "--control-fd") != 0 ||
      ct_nested_command_add_descriptor(command, "",
                                        request->control_descriptor) != 0) {
    return 1;
  }
  if (request->inherited_descriptor >= 3 &&
      (ct_nested_command_add(command, "--descriptor-fd") != 0 ||
       ct_nested_command_add_descriptor(command, "",
                                        request->inherited_descriptor) != 0)) {
    return 1;
  }
  if (probe != 0) {
    if (ct_nested_command_add_probe(request, command) != 0) return 1;
  } else {
    if (request->payload == NULL) return 1;
    for (index = 0U; request->payload[index] != NULL; ++index) {
      if (ct_nested_command_add(command, request->payload[index]) != 0) return 1;
    }
  }
  return ct_nested_command_finish(command);
}

int ct_backend_nested_requires_read_only(const struct ct_nested_request *request)
{
  size_t index;
  if (request == NULL || request->profile == NULL || request->map == NULL) return 1;
  if (strcmp(request->profile->root_access, "read-only") == 0) return 1;
  for (index = 0U; index < request->map->count; ++index) {
    if (strcmp(request->map->entries[index].access, "read-only") == 0) return 1;
  }
  return 0;
}

static int ct_nested_forced(const char *forced, enum ct_nested_backend backend)
{
  const char *name = ct_backend_nested_name(backend);
  return forced == NULL || (name != NULL && strcmp(forced, name) == 0);
}

static const char *ct_nested_tool(const struct ct_nested_request *request,
                                   enum ct_nested_backend backend)
{
  if (backend == CT_NESTED_BACKEND_BUBBLEWRAP) {
    return request->bubblewrap_path == NULL ? "bwrap" : request->bubblewrap_path;
  }
  if (backend == CT_NESTED_BACKEND_PROOT) {
    return request->proot_path == NULL ? "proot" : request->proot_path;
  }
  return NULL;
}

static int ct_nested_trampoline_admissible(const struct ct_nested_request *request)
{
  struct stat descriptor;
  struct stat executable;
  return request == NULL || request->trampoline_descriptor < 0 ||
                 fstat(request->trampoline_descriptor, &descriptor) != 0 ||
                 stat("/proc/self/exe", &executable) != 0 ||
                 !S_ISREG(descriptor.st_mode) ||
                 descriptor.st_dev != executable.st_dev ||
                 descriptor.st_ino != executable.st_ino
             ? 1
             : 0;
}

static int ct_nested_absolute_tool(const char *candidate,
                                   char resolved[CT_HOST_PATH_MAX])
{
  char cwd[CT_HOST_PATH_MAX];
  int written;
  if (candidate == NULL || candidate[0] == '\0' || resolved == NULL) return 1;
  if (candidate[0] == '/') {
    written = snprintf(resolved, CT_HOST_PATH_MAX, "%s", candidate);
  } else {
    if (getcwd(cwd, sizeof(cwd)) == NULL) return 1;
    written = snprintf(resolved, CT_HOST_PATH_MAX, "%s/%s", cwd, candidate);
  }
  return written < 0 || (size_t)written >= CT_HOST_PATH_MAX;
}

static int ct_nested_resolve_tool(const char *tool,
                                  char resolved[CT_HOST_PATH_MAX])
{
  const char *path;
  const char *cursor;
  if (tool == NULL || tool[0] == '\0' || resolved == NULL) return 1;
  if (strchr(tool, '/') != NULL) {
    return ct_nested_absolute_tool(tool, resolved) != 0 ||
           access(resolved, X_OK) != 0;
  }
  path = getenv("PATH");
  if (path == NULL) return 1;
  cursor = path;
  for (;;) {
    const char *end = strchr(cursor, ':');
    const size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
    char candidate[CT_HOST_PATH_MAX];
    const char *directory = length == 0U ? "." : cursor;
    const int written = length == 0U
                             ? snprintf(candidate, sizeof(candidate), "./%s", tool)
                             : snprintf(candidate, sizeof(candidate), "%.*s/%s",
                                        (int)length, directory, tool);
    if (written > 0 && (size_t)written < sizeof(candidate) &&
        ct_nested_absolute_tool(candidate, resolved) == 0 &&
        access(resolved, X_OK) == 0) {
      return 0;
    }
    if (end == NULL) return 1;
    cursor = end + 1;
  }
}

static enum ct_nested_build_result ct_nested_build(
    enum ct_nested_backend backend, const struct ct_nested_request *request,
    int probe, struct ct_nested_command *command)
{
  size_t payload_count = 0U;
  size_t capacity;
  if (command == NULL) return CT_NESTED_BUILD_INTERNAL_FAILURE;
  memset(command, 0, sizeof(*command));
  if (probe == 0 && request->payload != NULL) {
    while (request->payload[payload_count] != NULL &&
           payload_count < CT_NESTED_PAYLOAD_LIMIT) {
      ++payload_count;
    }
    if (payload_count == CT_NESTED_PAYLOAD_LIMIT) {
      return CT_NESTED_BUILD_INTERNAL_FAILURE;
    }
  }
  capacity = backend == CT_NESTED_BACKEND_BUBBLEWRAP
                   ? 64U + CT_NESTED_ROOT_ENTRY_LIMIT * 3U +
                         request->map->count * 5U + payload_count
                  : backend == CT_NESTED_BACKEND_PROOT
                         ? 22U + request->map->count * 2U + payload_count
                        : 16U + request->executable->stage_count *
                                    (2U + CT_EXECUTABLE_ENV_ARGUMENT_MAX) +
                              payload_count;
  if (capacity > CT_NESTED_ARGUMENT_LIMIT ||
      ct_nested_command_init(command, capacity) != 0) {
    return CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  if (backend == CT_NESTED_BACKEND_BUBBLEWRAP) {
    return ct_backend_bubblewrap_arguments(request, probe, command);
  }
  if (backend == CT_NESTED_BACKEND_PROOT) {
    return ct_backend_proot_arguments(request, probe, command) == 0
               ? CT_NESTED_BUILD_OK
               : CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  return probe == 0 && ct_backend_rewrite_arguments(request, command) == 0
             ? CT_NESTED_BUILD_OK
             : CT_NESTED_BUILD_INTERNAL_FAILURE;
}

static int ct_nested_probe(enum ct_nested_backend backend,
                            const struct ct_nested_request *request,
                            int *trustworthy, int *fallback_allowed)
{
  struct ct_nested_command command = {0};
  struct ct_supervisor_probe_result result;
  enum ct_supervisor_probe_completion completion;
  if (trustworthy == NULL || fallback_allowed == NULL) return 125;
  *trustworthy = 0;
  *fallback_allowed = 0;
  {
    const enum ct_nested_build_result build_result =
        ct_nested_build(backend, request, 1, &command);
    if (build_result != CT_NESTED_BUILD_OK) {
      *trustworthy = build_result == CT_NESTED_BUILD_UNSUPPORTED;
      *fallback_allowed = build_result == CT_NESTED_BUILD_UNSUPPORTED;
      ct_nested_command_destroy(&command);
      return 125;
    }
  }
#ifdef CT_SUPERVISOR_TEST_SEAM
  if (getenv("CT_TEST_NESTED_INFRASTRUCTURE_FAILURE") != NULL) {
    ct_nested_command_destroy(&command);
    return 125;
  }
#endif
  completion = ct_supervisor_probe_environment_detailed(
      command.arguments, 5000U, request->environment, request->environment_count,
      &result);
  ct_nested_command_destroy(&command);
  if (completion != CT_SUPERVISOR_PROBE_COMPLETED ||
      result.infrastructure_failed != 0) {
    return 125;
  }
  *trustworthy = 1;
  *fallback_allowed = result.interrupted == 0;
  return result.status;
}

static int ct_nested_eligible(enum ct_nested_backend backend,
                              const struct ct_nested_request *request,
                              int allow_rewrite)
{
  if (strcmp(request->profile->semantics, "rewrite") == 0) {
    return backend == CT_NESTED_BACKEND_REWRITE &&
           ct_backend_nested_requires_read_only(request) == 0;
  }
  if (backend == CT_NESTED_BACKEND_BUBBLEWRAP) return 1;
  if (backend == CT_NESTED_BACKEND_PROOT) {
    return ct_backend_nested_requires_read_only(request) == 0;
  }
  return allow_rewrite != 0 &&
         ct_backend_nested_requires_read_only(request) == 0;
}

static const char *ct_nested_probe_reason(int result)
{
  if (result == 124) return "probe-timeout";
  if (result == 126) return "policy-denied";
  return "probe-failed";
}

int ct_backend_nested_diagnose(
    const struct ct_nested_request *request, int allow_rewrite,
    struct ct_nested_backend_report reports[CT_NESTED_BACKEND_COUNT])
{
  enum ct_nested_backend backend;
  int earlier_ready = 0;
  if (request == NULL || request->profile == NULL || request->map == NULL ||
      reports == NULL || request->trampoline_descriptor < 0 ||
      request->control_descriptor < 0) {
    return 125;
  }
  memset(reports, 0, CT_NESTED_BACKEND_COUNT * sizeof(*reports));
  for (backend = CT_NESTED_BACKEND_BUBBLEWRAP;
       backend <= CT_NESTED_BACKEND_REWRITE;
       backend = (enum ct_nested_backend)(backend + 1)) {
    struct ct_nested_backend_report *report = &reports[(size_t)backend];
    char tool[CT_HOST_PATH_MAX];
    int probe;
    report->backend = backend;
    report->installed = backend == CT_NESTED_BACKEND_REWRITE
                             ? 1
                             : ct_nested_resolve_tool(
                                   ct_nested_tool(request, backend), tool) == 0;
    report->eligible = ct_nested_eligible(backend, request, allow_rewrite);
    report->operational = CT_NESTED_OPERATIONAL_NOT_PROBED;
    if (report->eligible == 0) {
      report->reason_code = backend == CT_NESTED_BACKEND_REWRITE
                                ? "degradation-not-authorized"
                                : "incompatible-profile";
      continue;
    }
    if (earlier_ready != 0) {
      report->reason_code = "earlier-backend-ready";
      continue;
    }
    if (report->installed == 0) {
      report->operational = CT_NESTED_OPERATIONAL_NO;
      report->reason_code = "not-installed";
      continue;
    }
    if (backend == CT_NESTED_BACKEND_REWRITE) {
      report->operational = CT_NESTED_OPERATIONAL_YES;
      report->reason_code = "ready";
      continue;
    }
    {
      struct ct_nested_request frozen = *request;
      int fallback_allowed;
      int trustworthy;
      if (backend == CT_NESTED_BACKEND_BUBBLEWRAP) frozen.bubblewrap_path = tool;
      else frozen.proot_path = tool;
      probe = ct_nested_probe(backend, &frozen, &trustworthy, &fallback_allowed);
      if (trustworthy == 0) return 125;
    }
    if (probe == 0) {
      report->operational = CT_NESTED_OPERATIONAL_YES;
      report->reason_code = "ready";
      earlier_ready = 1;
    } else {
      report->operational = CT_NESTED_OPERATIONAL_NO;
      report->reason_code = ct_nested_probe_reason(probe);
    }
  }
  return 0;
}

int ct_backend_nested_blocked(
    const struct ct_nested_request *request, const char *reason_code,
    struct ct_nested_backend_report reports[CT_NESTED_BACKEND_COUNT])
{
  enum ct_nested_backend backend;
  if (request == NULL || request->profile == NULL || reports == NULL ||
      reason_code == NULL) {
    return 125;
  }
  memset(reports, 0, CT_NESTED_BACKEND_COUNT * sizeof(*reports));
  for (backend = CT_NESTED_BACKEND_BUBBLEWRAP;
       backend <= CT_NESTED_BACKEND_REWRITE;
       backend = (enum ct_nested_backend)(backend + 1)) {
    struct ct_nested_backend_report *report = &reports[(size_t)backend];
    report->backend = backend;
    {
      char tool[CT_HOST_PATH_MAX];
      report->installed = backend == CT_NESTED_BACKEND_REWRITE
                              ? 1
                              : ct_nested_resolve_tool(
                                        ct_nested_tool(request, backend), tool) ==
                                    0;
    }
    report->eligible = 0;
    report->operational = CT_NESTED_OPERATIONAL_NOT_PROBED;
    report->reason_code = reason_code;
  }
  return 0;
}

int ct_backend_nested_execute_detailed(
    const struct ct_nested_request *request, const char *forced,
    int allow_rewrite, enum ct_nested_pre_dispatch_failure *failure)
{
  enum ct_nested_backend backend;
  enum ct_nested_pre_dispatch_failure last_failure =
      CT_NESTED_PRE_DISPATCH_NONE;
  struct ct_nested_backend_report outcomes[CT_NESTED_BACKEND_COUNT] = {{0}};
  if (failure != NULL) *failure = CT_NESTED_PRE_DISPATCH_NONE;
  if (request == NULL || request->profile == NULL || request->map == NULL ||
      request->payload == NULL || request->payload[0] == NULL ||
      request->control_descriptor < 0 ||
      ct_nested_trampoline_admissible(request) != 0) {
    if (failure != NULL) *failure = CT_NESTED_PRE_DISPATCH_TRAMPOLINE;
    return 125;
  }
  for (backend = CT_NESTED_BACKEND_BUBBLEWRAP;
       backend <= CT_NESTED_BACKEND_REWRITE;
       backend = (enum ct_nested_backend)(backend + 1)) {
    outcomes[(size_t)backend].backend = backend;
    outcomes[(size_t)backend].reason_code = "not-attempted";
  }
  for (backend = CT_NESTED_BACKEND_BUBBLEWRAP;
       backend <= CT_NESTED_BACKEND_REWRITE;
       backend = (enum ct_nested_backend)(backend + 1)) {
    struct ct_nested_command command = {0};
    struct ct_nested_request frozen;
    char tool[CT_HOST_PATH_MAX];
    int probe;
    int result;
    if (!ct_nested_forced(forced, backend)) continue;
    if (ct_nested_eligible(backend, request, allow_rewrite) == 0) {
      outcomes[(size_t)backend].reason_code =
          strcmp(request->profile->semantics, "rewrite") == 0
              ? "not-requested"
              : "incompatible-profile";
      last_failure = CT_NESTED_PRE_DISPATCH_POLICY_DENIED;
      if (forced != NULL) {
        if (failure != NULL) *failure = last_failure;
        return 125;
      }
      continue;
    }
    frozen = *request;
    if (backend != CT_NESTED_BACKEND_REWRITE) {
      if (ct_nested_resolve_tool(ct_nested_tool(request, backend), tool) != 0) {
        outcomes[(size_t)backend].installed = 0;
        outcomes[(size_t)backend].eligible = 1;
        outcomes[(size_t)backend].operational = CT_NESTED_OPERATIONAL_NO;
        outcomes[(size_t)backend].reason_code = "not-installed";
        last_failure = CT_NESTED_PRE_DISPATCH_TOOL_MISSING;
        if (forced != NULL) {
          if (failure != NULL) *failure = last_failure;
          return 125;
        }
        continue;
      }
      outcomes[(size_t)backend].installed = 1;
      outcomes[(size_t)backend].eligible = 1;
      if (backend == CT_NESTED_BACKEND_BUBBLEWRAP) frozen.bubblewrap_path = tool;
      else frozen.proot_path = tool;
    }
    if (backend != CT_NESTED_BACKEND_REWRITE) {
      int fallback_allowed;
      int trustworthy;
      probe = ct_nested_probe(backend, &frozen, &trustworthy, &fallback_allowed);
      if (probe != 0) {
        outcomes[(size_t)backend].operational = CT_NESTED_OPERATIONAL_NO;
        outcomes[(size_t)backend].reason_code = ct_nested_probe_reason(probe);
        last_failure = trustworthy == 0
                           ? CT_NESTED_PRE_DISPATCH_CLEANUP_UNCERTAIN
                           : probe == 126 ? CT_NESTED_PRE_DISPATCH_POLICY_DENIED
                                          : probe == 124 ? CT_NESTED_PRE_DISPATCH_PROBE_TIMEOUT
                                          : CT_NESTED_PRE_DISPATCH_PROBE_FAILED;
        if (forced != NULL || trustworthy == 0) {
          if (failure != NULL) *failure = last_failure;
          return 125;
        }
        if (fallback_allowed == 0) return probe;
        continue;
      }
    }
    if (ct_nested_build(backend, &frozen, 0, &command) != 0) {
      ct_nested_command_destroy(&command);
      if (failure != NULL) *failure = CT_NESTED_PRE_DISPATCH_COMMAND_BUILD;
      return 125;
    }
    if (backend == CT_NESTED_BACKEND_REWRITE &&
        ct_backend_rewrite_warning(stderr, request->profile->semantics,
                                   outcomes) != 0) {
      ct_nested_command_destroy(&command);
      if (failure != NULL) *failure = CT_NESTED_PRE_DISPATCH_DIAGNOSTIC_OUTPUT;
      return 125;
    }
    result = ct_process_exec(command.arguments, request->environment,
                            request->environment_count);
    ct_nested_command_destroy(&command);
    return result;
  }
  if (failure != NULL) {
    *failure = last_failure == CT_NESTED_PRE_DISPATCH_NONE
                   ? CT_NESTED_PRE_DISPATCH_POLICY_DENIED
                   : last_failure;
  }
  return 125;
}

int ct_backend_nested_execute(const struct ct_nested_request *request,
                               const char *forced, int allow_rewrite)
{
  return ct_backend_nested_execute_detailed(request, forced, allow_rewrite,
                                            NULL);
}
