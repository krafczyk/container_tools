/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_STATE_H
#define CONTAINER_TOOLS_STATE_H

#include <stddef.h>

#define CT_STATE_PATH_MAX 4096U

/** Exact pending record stored while an instance start is unresolved. */
struct ct_state_pending {
  char name[41];
  char profile[66];
  char nonce[34];
};

/** Distinguishes a contended profile lock from invalid or inaccessible state. */
enum ct_state_lock_status {
  CT_STATE_LOCK_OK = 0,
  CT_STATE_LOCK_SETUP = 1,
  CT_STATE_LOCK_TIMEOUT = 2,
};
/** Outcome of resolving a managed instance-name prefix in private state. */
enum ct_state_identity_selector_status {
  CT_STATE_IDENTITY_SELECTOR_OK = 0,
  CT_STATE_IDENTITY_SELECTOR_ABSENT,
  CT_STATE_IDENTITY_SELECTOR_AMBIGUOUS,
  CT_STATE_IDENTITY_SELECTOR_IO,
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
 * Atomically publish required mode-0600 metadata beside a ready instance.
 *
 * Returns nonzero when validated metadata cannot be written.
 */
int ct_state_identity_write(const char *root, const char *name, const char *image,
                            const char *image_identity, const char *profile);
/**
 * Read the established mode-0600 identity index without trusting its filename.
 *
 * @param root Private instance root.
 * @param name Exact managed instance name.
 * @param profile Receives the exact full profile digest.
 * @return Zero only for a complete compatible matching record.
 */
int ct_state_identity_read(const char *root, const char *name, char profile[65]);
/**
 * Resolve exactly one private identity index beginning with a hash prefix.
 *
 * @param root Existing private instance root.
 * @param prefix Nonempty lowercase hexadecimal prefix no longer than 32 bytes.
 * @param name Receives the matched full managed instance name when unique.
 * @return A selector outcome; no name is returned for absent or ambiguous input.
 */
enum ct_state_identity_selector_status ct_state_identity_resolve_prefix(
    const char *root, const char *prefix, char name[40]);
/** Validate or create the current-user-owned mode-0700 instance root. */
int ct_state_prepare_root(const char *root);
/**
 * Acquire one profile lock within `CT_INSTANCE_LOCK_TIMEOUT`.
 *
 * Creates the validated root when needed and returns its held descriptor through
 * `descriptor`. Returns CT_STATE_LOCK_TIMEOUT for contention and
 * CT_STATE_LOCK_SETUP for invalid state, I/O failure, or invalid timeout setup.
 */
enum ct_state_lock_status ct_state_lock(const char *root, const char *name,
                                        int *descriptor);
/** Release and close a descriptor returned by `ct_state_lock`; negative is a no-op. */
void ct_state_unlock(int descriptor);

#endif
