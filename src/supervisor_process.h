/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_SUPERVISOR_PROCESS_H
#define CONTAINER_TOOLS_SUPERVISOR_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/** Maximum number of process identities retained for one forced cleanup. */
#define CT_SUPERVISOR_PROCESS_CAPACITY 16384U

/** Pinned process identities owned by one forced supervisor cleanup. */
struct ct_supervisor_process_owned {
  /** Numeric PIDs used only to locate later adoption candidates. */
  pid_t pids[CT_SUPERVISOR_PROCESS_CAPACITY];
  /** Procfs start times paired with the pinned identities. */
  unsigned long long start_times[CT_SUPERVISOR_PROCESS_CAPACITY];
  /** Pidfds that provide signaling authority and prevent identity substitution. */
  int pidfds[CT_SUPERVISOR_PROCESS_CAPACITY];
  /** Number of initialized entries in each parallel array. */
  size_t count;
};

/** Per-run controls for deterministic forced-cleanup failure tests. */
struct ct_supervisor_process_options {
  /** Simulate pidfd_open unavailability when CT_SUPERVISOR_TEST_SEAM is set. */
  bool force_pidfd_unavailable;
  /** Fail after one pinned descendant when CT_SUPERVISOR_TEST_SEAM is set. */
  bool fail_after_first_descendant;
};

/**
 * Read the monotonic clock used by supervisor deadlines.
 *
 * @return Monotonic milliseconds, or -1 when the clock cannot be read.
 */
long long ct_supervisor_process_now(void);

/**
 * Pause for at most one polling interval without crossing a deadline.
 *
 * @param deadline Absolute monotonic deadline in milliseconds.
 * @return Zero after a completed or interrupted pause, otherwise nonzero.
 */
int ct_supervisor_process_pause(long long deadline);

/**
 * Initialize an empty owned-process collection.
 *
 * @param owned Collection whose count and descriptor ownership are reset.
 */
void ct_supervisor_process_owned_init(struct ct_supervisor_process_owned *owned);

/**
 * Signal every direct child created by any thread of one private supervisor.
 *
 * The caller must be the sole reaper and must not reap between enumeration and
 * this call's numeric-PID signals, which prevents PID reuse on this normal path.
 *
 * @param parent Private supervisor process whose children are enumerated.
 * @param signal_number Signal delivered to each direct child.
 * @param deadline Absolute monotonic deadline for procfs reads.
 * @return Zero on success, otherwise nonzero.
 */
int ct_supervisor_process_signal_children(pid_t parent, int signal_number,
                                          long long deadline);

/**
 * Freeze and pin every descendant of a private supervisor.
 *
 * Every process is opened with pidfd_open before its start time is validated or
 * any signal is sent. Children are enumerated from every thread. On failure,
 * identities pinned before the failure remain in @p owned for later cleanup.
 *
 * @param root Private supervisor PID, still owned and unreaped by the caller.
 * @param owned Destination for pinned descendant identities.
 * @param deadline Absolute monotonic deadline for collection.
 * @param options Per-run test controls; production callers pass false fields.
 * @return Zero for a complete snapshot, otherwise nonzero.
 */
int ct_supervisor_process_collect_descendants(
    pid_t root, struct ct_supervisor_process_owned *owned, long long deadline,
    const struct ct_supervisor_process_options *options);

/**
 * Signal an anchored supervisor group or a pidfd-pinned ungrouped supervisor.
 *
 * @param supervisor Unreaped private supervisor PID/PGID anchor.
 * @param group_established Whether negative-PGID signaling is safe.
 * @param signal_number Signal to deliver.
 * @param deadline Absolute monotonic deadline for identity validation.
 * @param options Per-run test controls; production callers pass false fields.
 * @return Zero on success, otherwise -1 with errno set.
 */
int ct_supervisor_process_signal_anchor(
    pid_t supervisor, bool group_established, int signal_number,
    long long deadline, const struct ct_supervisor_process_options *options);

/**
 * Kill and reap only pinned descendants adopted after supervisor death.
 *
 * Current children are used only to detect adoption. Start time and pidfd
 * liveness are revalidated before SIGKILL, and unrecognized children are never
 * signaled or reaped.
 *
 * @param owned Pinned identities obtained during descendant collection.
 * @param deadline Absolute monotonic cleanup deadline.
 * @return Zero after all available proven descendants are gone, otherwise
 *         nonzero.
 */
int ct_supervisor_process_cleanup_adopted(
    const struct ct_supervisor_process_owned *owned, long long deadline);

/**
 * Close every pidfd retained by an owned-process collection.
 *
 * All descriptors are attempted independently.
 *
 * @param owned Collection to close and invalidate.
 * @return Zero when every close succeeds, otherwise nonzero.
 */
int ct_supervisor_process_owned_close(
    struct ct_supervisor_process_owned *owned);

#endif
