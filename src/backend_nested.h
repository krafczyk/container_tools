/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_BACKEND_NESTED_H
#define CONTAINER_TOOLS_BACKEND_NESTED_H

#include "executable.h"
#include "path_map.h"
#include "process.h"

#define CT_NESTED_ARGUMENT_LIMIT (CT_PATH_MAP_MAX_ENTRIES * 3U + 32U)
#define CT_NESTED_DETAIL_MAX 8193U

/** Closed nested-backend selection order. */
enum ct_nested_backend {
  CT_NESTED_BACKEND_BUBBLEWRAP = 0,
  CT_NESTED_BACKEND_PROOT = 1,
  CT_NESTED_BACKEND_REWRITE = 2,
  CT_NESTED_BACKEND_COUNT = 3
};
/** Fully prepared selected-root execution request. */
struct ct_nested_request {
  const struct ct_host_profile *profile;
  const struct ct_path_map *map;
  const struct ct_executable *executable;
  const char *cwd;
  char *const *payload;
  const struct ct_process_environment *environment;
  size_t environment_count;
  int trampoline_descriptor;
  int control_descriptor;
  int inherited_descriptor;
  const char *bubblewrap_path;
  const char *proot_path;
};
/** Owned argv and formatted values for one bounded backend operation. */
struct ct_nested_command {
  char **arguments;
  char **owned;
  size_t count;
  size_t owned_count;
  size_t capacity;
};
/** One closed capability result used by selection and doctor output. */
enum ct_nested_operational {
  CT_NESTED_OPERATIONAL_NO = 0,
  CT_NESTED_OPERATIONAL_YES = 1,
  CT_NESTED_OPERATIONAL_NOT_PROBED = 2
};
/** Closed actionable reason for a failure before a backend dispatch begins. */
enum ct_nested_pre_dispatch_failure {
  CT_NESTED_PRE_DISPATCH_NONE = 0,
  CT_NESTED_PRE_DISPATCH_TOOL_MISSING,
  CT_NESTED_PRE_DISPATCH_POLICY_DENIED,
  CT_NESTED_PRE_DISPATCH_PROBE_TIMEOUT,
  CT_NESTED_PRE_DISPATCH_PROBE_FAILED,
  CT_NESTED_PRE_DISPATCH_CLEANUP_UNCERTAIN,
  CT_NESTED_PRE_DISPATCH_TRAMPOLINE,
  CT_NESTED_PRE_DISPATCH_COMMAND_BUILD,
  CT_NESTED_PRE_DISPATCH_DIAGNOSTIC_OUTPUT
};
/** One closed capability result used by selection and doctor output. */
struct ct_nested_backend_report {
  enum ct_nested_backend backend;
  int installed;
  int eligible;
  enum ct_nested_operational operational;
  const char *reason_code;
  char detail[CT_NESTED_DETAIL_MAX];
};
/** Return the stable public name for one nested backend. */
const char *ct_backend_nested_name(enum ct_nested_backend backend);
/** Initialize an empty heap-backed backend command. */
int ct_nested_command_init(struct ct_nested_command *command, size_t capacity);
/** Append one borrowed argument. */
int ct_nested_command_add(struct ct_nested_command *command, const char *value);
/** Append one owned argument formed as `first + separator + second`. */
int ct_nested_command_add_pair(struct ct_nested_command *command,
                               const char *first, const char *separator,
                               const char *second);
/** Append one owned decimal descriptor after a literal prefix. */
int ct_nested_command_add_descriptor(struct ct_nested_command *command,
                                     const char *prefix, int descriptor);
/** Finish a command with a NULL argv terminator. */
int ct_nested_command_finish(struct ct_nested_command *command);
/** Release all storage owned by a backend command. */
void ct_nested_command_destroy(struct ct_nested_command *command);
/** Append the shared descriptor trampoline and probe or payload suffix. */
int ct_nested_command_add_trampoline(const struct ct_nested_request *request,
                                     int probe,
                                     struct ct_nested_command *command);
/**
 * Probe eligible backends in fixed order and dispatch exactly one selected backend.
 *
 * A forced backend never falls back. Full-root rewrite requires explicit consent;
 * a rewrite profile is itself that consent. Probe failure completes before a lower
 * backend is tried. After a successful probe, the selected launch is attempted once
 * and its exact result is returned without fallback.
 *
 * @param request Fully prepared selected-root request with live descriptors.
 * @param forced Optional `bubblewrap`, `proot`, or `rewrite` selector.
 * @param allow_rewrite Nonzero only for explicit full-root degradation consent.
 * @return Exact dispatched status, or 125 before dispatch when none can run.
 */
int ct_backend_nested_execute(const struct ct_nested_request *request,
                               const char *forced, int allow_rewrite);
/**
 * Dispatch one selected backend while retaining a pre-dispatch failure class.
 *
 * Payload and backend launch statuses retain their exact protocol value and set
 * `failure` to `CT_NESTED_PRE_DISPATCH_NONE`; only failures before dispatch
 * produce a non-none class.
 *
 * @param request Fully prepared selected-root request with live descriptors.
 * @param forced Optional fixed backend selector.
 * @param allow_rewrite Nonzero only for explicit full-root degradation consent.
 * @param failure Destination for a closed pre-dispatch reason, if non-NULL.
 * @return Exact dispatched status or 125 when dispatch cannot begin.
 */
int ct_backend_nested_execute_detailed(
    const struct ct_nested_request *request, const char *forced,
    int allow_rewrite, enum ct_nested_pre_dispatch_failure *failure);
/**
 * Diagnose nested backends in fixed order without dispatching a payload.
 *
 * Probing stops after the first operational full-root backend; later entries
 * receive `earlier-backend-ready`. Rewrite is a policy capability and is not
 * executed by doctor. Every started probe is fully reaped before return.
 *
 * @param request Prepared root, mapping, descriptor, and environment plan.
 * @param allow_rewrite Invocation-level full-root degradation consent.
 * @param reports Three caller-owned entries in fixed backend order.
 * @return Zero for a complete diagnosis or 125 for an internal/probe-cleanup
 *         failure that prevents a trustworthy complete report.
 */
int ct_backend_nested_diagnose(
    const struct ct_nested_request *request, int allow_rewrite,
    struct ct_nested_backend_report reports[CT_NESTED_BACKEND_COUNT]);
/** Populate fixed-order non-probed reports for an earlier planning failure. */
int ct_backend_nested_blocked(
    const struct ct_nested_request *request, const char *reason_code,
    struct ct_nested_backend_report reports[CT_NESTED_BACKEND_COUNT]);
/** Determine whether a profile requires container-tools-enforced read-only access. */
int ct_backend_nested_requires_read_only(const struct ct_nested_request *request);
#endif
