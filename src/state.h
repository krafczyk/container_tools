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
/** Outcome of resolving a managed instance-name prefix in instance state. */
enum ct_state_identity_selector_status {
  CT_STATE_IDENTITY_SELECTOR_OK = 0,
  CT_STATE_IDENTITY_SELECTOR_ABSENT,
  CT_STATE_IDENTITY_SELECTOR_AMBIGUOUS,
  CT_STATE_IDENTITY_SELECTOR_IO,
};

/**
 * Construct the pending-record path for a validated root and instance name.
 *
 * @param root Absolute normalized managed instance root.
 * @param name Exact validated managed instance name.
 * @param path Receives ROOT/NAME.pending.
 * @return Zero on success; nonzero for invalid or oversized input.
 */
int ct_state_pending_path(const char *root, const char *name,
                          char path[CT_STATE_PATH_MAX]);
/**
 * Atomically write the established three-line pending record.
 *
 * New records request mode 0600. Existing managed state is admitted by path,
 * type, and content rather than reported ownership or permission bits.
 *
 * @param path Validated pending-record destination.
 * @param name Exact managed instance name.
 * @param profile Lowercase 64-hex profile digest.
 * @param nonce Lowercase 32-hex creation nonce.
 * @return Zero after atomic publication; nonzero without publishing partial
 *         bytes when validation or storage fails.
 * @sideeffect Creates, synchronizes, and renames one temporary record beside
 *             the destination.
 */
int ct_state_pending_write(const char *path, const char *name, const char *profile,
                           const char *nonce);
/**
 * Read and validate the exact pending record without mutating it.
 *
 * The record must be a regular file with the requested name/profile and one
 * lowercase 32-hex nonce. Returns nonzero for missing, malformed, future, or
 * mismatched state.
 *
 * @param path Existing pending-record path.
 * @param name Expected managed instance name.
 * @param profile Expected lowercase 64-hex profile digest.
 * @param record Receives the validated record.
 * @return Zero on a complete match; nonzero without mutating the record.
 */
int ct_state_pending_read(const char *path, const char *name, const char *profile,
                          struct ct_state_pending *record);
/**
 * Remove a verified pending record; an already absent record is successful.
 *
 * @param path Pending-record path to remove.
 * @return Zero after removal or when absent; nonzero on storage failure.
 * @sideeffect Unlinks the selected pending record when present.
 */
int ct_state_pending_clear(const char *path);
/**
 * Atomically publish required metadata beside a ready instance.
 *
 * @param root Absolute normalized managed instance root.
 * @param name Exact managed instance name.
 * @param image Canonical image path without newlines.
 * @param image_identity Validated image identity without newlines.
 * @param profile Lowercase 64-hex profile digest.
 * @return Zero after atomic publication; nonzero on validation or storage failure.
 * @sideeffect Creates, synchronizes, and renames one temporary identity index.
 */
int ct_state_identity_write(const char *root, const char *name, const char *image,
                            const char *image_identity, const char *profile);
/**
 * Read the established identity index without trusting its filename.
 *
 * @param root Managed instance root.
 * @param name Exact managed instance name.
 * @param profile Receives the exact full profile digest.
 * @return Zero only for a complete compatible matching record.
 * @sideeffect Reads but does not mutate managed instance state.
 */
int ct_state_identity_read(const char *root, const char *name, char profile[65]);
/**
 * Resolve exactly one identity index beginning with a hash prefix.
 *
 * @param root Existing managed instance root.
 * @param prefix Nonempty lowercase hexadecimal prefix no longer than 32 bytes.
 * @param name Receives the matched full managed instance name when unique.
 * @return A selector outcome; no name is returned for absent or ambiguous input.
 * @sideeffect Enumerates but does not mutate managed instance state.
 */
enum ct_state_identity_selector_status ct_state_identity_resolve_prefix(
    const char *root, const char *prefix, char name[40]);
/**
 * Validate or create one real instance root without following symlinks.
 *
 * New directories request mode 0700; ownership and mode are not admission
 * criteria for an existing real directory.
 *
 * @param root Absolute normalized managed instance root.
 * @return Zero for a real directory created or adopted without symlinks;
 *         nonzero for invalid path or storage state.
 * @sideeffect May create root and missing real-directory ancestors.
 */
int ct_state_prepare_root(const char *root);
/**
 * Acquire one profile lock within `CT_INSTANCE_LOCK_TIMEOUT`.
 *
 * Creates the validated root and a mode-0600 lock when needed, then returns its
 * held descriptor through `descriptor`. Existing ownership and mode
 * presentation are not admission criteria. Returns CT_STATE_LOCK_TIMEOUT for contention and
 * CT_STATE_LOCK_SETUP for invalid state, I/O failure, or invalid timeout setup.
 * Existing lock paths must resolve directly to regular files.
 *
 * @param root Absolute normalized managed instance root.
 * @param name Exact managed instance name selecting the lock.
 * @param descriptor Receives the held lock descriptor on success.
 * @return A typed success, setup-failure, or contention-timeout status.
 * @sideeffect May create the root and lock file and holds an exclusive flock.
 */
enum ct_state_lock_status ct_state_lock(const char *root, const char *name,
                                        int *descriptor);
/**
 * Release and close a descriptor returned by `ct_state_lock`.
 *
 * @param descriptor Held lock descriptor; a negative value is a no-op.
 * @sideeffect Releases the advisory lock and closes the descriptor.
 */
void ct_state_unlock(int descriptor);

#endif
