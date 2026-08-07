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
 * Initialize a complete selection that exposes no host projection.
 *
 * Clears all prior entries and derives the group mode from `backend`. Returns
 * nonzero when the backend is unsupported or `selection` is null.
 */
int ct_host_projection_set_none(const char *backend,
                                struct ct_host_projection *selection);
/**
 * Select and revalidate a bounded local host projection.
 *
 * `mode` must be `auto`, `required`, or `disabled`; refresh bypasses one warm
 * record.  The function never dispatches a payload.  It returns zero for an
 * available local selection (including `none`), and nonzero for unavailable
 * endpoints or failed selection. Malformed cache state is treated as a cold
 * miss and replaced only after a complete current selection is proven. The caller applies the
 * required-mode complete-selection policy before constructing payload argv.
 */
int ct_host_projection_prepare(const char *backend, const char *image, const char *mode, int refresh, struct ct_host_projection *selection);

#endif
