/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_HOST_PROJECTION_H
#define CONTAINER_TOOLS_HOST_PROJECTION_H

#include <stddef.h>

#define CT_HOST_PROJECTION_MAX_ENTRIES 128U

/** A resolved generated host-root bind and its canonical in-container destination. */
struct ct_host_projection_entry { char source[4096]; char target[4096]; char destination[4096]; };

/** The bounded host-root selection consumed by one foreground launcher. */
struct ct_host_projection {
  char strategy[9];
  char completeness[9];
  char group_mode[24];
  struct ct_host_projection_entry entries[CT_HOST_PROJECTION_MAX_ENTRIES];
  size_t entry_count;
};

/** Return nonzero only for a local outer-runtime endpoint eligible for host state. */
int ct_host_projection_endpoint_is_local(const char *backend);
/** Return nonzero for lexical host paths that must never be mirrored beneath /host. */
int ct_host_projection_source_is_eligible(const char *path, const char *filesystem);
/**
 * Derive the canonical profile-v7 selection-record key for one local launch.
 *
 * Hashes the effective endpoint, executable, host identity, image, runtime
 * options, requested group mode, boot identity, and sorted caller groups using
 * the established NUL-framed Bash-compatible representation. Returns nonzero
 * when any bounded effective input cannot be finalized.
 */
int ct_host_projection_cache_key(const char *backend, const char *image,
                                 char output[65]);
/**
 * Remove all managed host-projection selection records from the selected cache.
 *
 * An absent cache succeeds without creating it. Clearing serializes with cache
 * readers and publishers, retains synchronization files, and refuses to remove
 * anything when the cache contains an unexpected name, symlink, or file type.
 *
 * @return Zero when the selected cache is absent or fully cleared; otherwise
 *         nonzero for invalid configuration or unsafe/unavailable storage.
 * @sideeffect Removes managed selection and interrupted-publication records so
 *             a following launcher invocation performs a cold projection proof.
 */
int ct_host_projection_cache_clear(void);
/**
 * Initialize a complete selection that exposes no host projection.
 *
 * Clears all prior entries and derives the group mode from `backend`. Returns
 * nonzero when the backend is unsupported or `selection` is null.
 */
int ct_host_projection_set_none(const char *backend,
                                struct ct_host_projection *selection);
#ifdef CT_STORAGE_TIMEOUT_TEST_SEAM
/**
 * Resolve the managed cache root for deterministic test verification.
 *
 * @param output Receives the normalized selected cache root.
 * @return Zero on success; nonzero when no supported root fits.
 */
int ct_host_projection_test_cache_root(char output[4096]);
#endif
/**
 * Select and revalidate a bounded local host projection.
 *
 * `mode` must be `auto`, `required`, or `disabled`; refresh bypasses one warm
 * record.  The function never dispatches a payload.  It returns zero for an
 * available local selection (including `none`), and nonzero for unavailable
 * endpoints or failed selection. Malformed cache state is treated as a cold
 * miss and replaced only after a complete current selection is proven. Cache
 * directories and files request modes 0700 and 0600 when created; reported
 * ownership and permission bits are not admission criteria for managed cache
 * state. The caller applies the
 * required-mode complete-selection policy before constructing payload argv.
 *
 * @param backend Selected supported backend.
 * @param image Image selector included in the cache identity.
 * @param mode Projection policy: auto, required, or disabled.
 * @param refresh Nonzero to bypass a warm cache record for this selection.
 * @param selection Receives the complete bounded projection selection.
 * @return Zero for an available selection; nonzero for invalid input,
 *         unavailable endpoint, or failed selection.
 * @sideeffect May probe a local runtime and create, lock, read, recover, or
 *             atomically replace managed projection-cache state.
 */
int ct_host_projection_prepare(const char *backend, const char *image, const char *mode, int refresh, struct ct_host_projection *selection);

#endif
