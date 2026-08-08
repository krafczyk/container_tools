/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_SUPERVISOR_H
#define CONTAINER_TOOLS_SUPERVISOR_H

#include "process.h"

/** Whether a probe completed with all supervisor state cleaned and restored. */
enum ct_supervisor_probe_completion {
  CT_SUPERVISOR_PROBE_COMPLETED = 0,
  CT_SUPERVISOR_PROBE_INFRASTRUCTURE_FAILURE = 1
};
/** Exact child result plus the trustworthiness of the supervisor boundary. */
struct ct_supervisor_probe_result {
  int status;
  int infrastructure_failed;
  int interrupted;
};

/**
 * Run one no-side-effect probe in an owned process group with bounded cleanup.
 *
 * The caller becomes a child subreaper. A fresh supervisor remains the
 * process-group leader until it has reaped the probe and descendants, including
 * descendants that create another process group or session. Standard descriptors
 * and inherited non-close-on-exec descriptors are preserved. Deadline cleanup
 * and caller interruption use only the anchored group and the supervisor's
 * adopted children, escalate TERM to KILL within 500 ms, and complete reaping
 * before a successful return. The payload retains the caller's signal mask and
 * SIGCHLD disposition; the private supervisor unblocks cleanup signals, while
 * the caller's SIGCHLD disposition and mask are restored before return. Forced
 * parent-side adoption requires Linux pidfds and procfs; unavailable identity
 * proof fails with 125 without signaling an unverified process.
 *
 * @param arguments NUL-terminated argv for one probe.
 * @param timeout_milliseconds Positive bounded deadline, at most five seconds.
 * @return Probe status, 128 plus signal, 124 on timeout, or 125 on setup,
 *         cleanup, or signal-restoration failure. Caller SIGINT/SIGTERM is
 *         restored and re-raised after cleanup.
 */
int ct_supervisor_probe(char *const arguments[], unsigned int timeout_milliseconds);
/** Run a bounded probe with child-only environment removals and overrides. */
int ct_supervisor_probe_environment(
    char *const arguments[], unsigned int timeout_milliseconds,
    const struct ct_process_environment *environment,
    size_t environment_count);
/**
 * Run a bounded probe and distinguish a clean child result from infrastructure
 * failure. A completed result may itself have status 125 when the child exits
 * 125; only an infrastructure failure means setup, cleanup, or restoration was
 * not trustworthy.
 *
 * @param arguments NUL-terminated argv for one probe.
 * @param timeout_milliseconds Positive bounded deadline, at most five seconds.
 * @param environment Bounded child-only environment removals and overrides.
 * @param environment_count Number of environment entries.
 * @param result Receives the child status, infrastructure-failure indicator,
 *        and whether caller interruption ended the probe.
 * @return `CT_SUPERVISOR_PROBE_COMPLETED` after a clean, fully restored probe,
 *         otherwise `CT_SUPERVISOR_PROBE_INFRASTRUCTURE_FAILURE`.
 */
enum ct_supervisor_probe_completion ct_supervisor_probe_environment_detailed(
    char *const arguments[], unsigned int timeout_milliseconds,
    const struct ct_process_environment *environment, size_t environment_count,
    struct ct_supervisor_probe_result *result);

#endif
