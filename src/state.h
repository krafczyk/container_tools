/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_STATE_H
#define CONTAINER_TOOLS_STATE_H

#include <stddef.h>

#define CT_STATE_PATH_MAX 4096U

/** Exact profile-v3 pending record stored while an instance start is unresolved. */
struct ct_state_pending {
  char name[41];
  char profile[66];
  char nonce[34];
};

/** Construct the pending-record path for a validated root and instance name. */
int ct_state_pending_path(const char *root, const char *name,
                          char path[CT_STATE_PATH_MAX]);
/**
 * Atomically write the established mode-0600, three-line pending record.
 *
 * Returns nonzero without publishing partial bytes when the path, name,
 * profile, nonce, write, synchronization, or rename is invalid.
 */
int ct_state_pending_write(const char *path, const char *name, const char *profile,
                           const char *nonce);
/**
 * Read and validate the exact pending record without mutating it.
 *
 * The record must be a current-user mode-0600 regular file with the requested
 * name/profile and one lowercase 32-hex nonce. Returns nonzero for missing,
 * malformed, future, insecure, or mismatched state.
 */
int ct_state_pending_read(const char *path, const char *name, const char *profile,
                          struct ct_state_pending *record);
/** Remove a verified pending record; an already absent record is successful. */
int ct_state_pending_clear(const char *path);
/**
 * Atomically publish best-effort mode-0600 metadata beside a ready instance.
 *
 * Returns nonzero when validated metadata cannot be written; callers may warn
 * without invalidating an already verified runtime instance.
 */
int ct_state_identity_write(const char *root, const char *name, const char *image,
                            const char *image_identity, const char *profile);
/** Validate or create the current-user-owned mode-0700 instance root. */
int ct_state_prepare_root(const char *root);
/**
 * Acquire one profile lock within `CT_INSTANCE_LOCK_TIMEOUT`.
 *
 * Creates the validated root when needed and returns its held descriptor through
 * `descriptor`. Returns nonzero on invalid state, I/O failure, or timeout.
 */
int ct_state_lock(const char *root, const char *name, int *descriptor);
/** Release and close a descriptor returned by `ct_state_lock`; negative is a no-op. */
void ct_state_unlock(int descriptor);

#endif
