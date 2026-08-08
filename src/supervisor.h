/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_SUPERVISOR_H
#define CONTAINER_TOOLS_SUPERVISOR_H

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

#endif
