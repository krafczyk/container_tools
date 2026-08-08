/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_MOUNT_PLAN_H
#define CONTAINER_TOOLS_MOUNT_PLAN_H

#include <stddef.h>

#define CT_MOUNT_PLAN_MAX_ENTRIES 4096U
#define CT_MOUNT_PLAN_MAX_BYTES 1048576U

/** One closed semantic mount-plan tuple in launcher assembly order. */
struct ct_mount_plan_entry { const char *role; const char *caller_path; const char *target_path; const char *access; const char *recursion; };
/** Complete major-1 semantic mount plan. Strings are borrowed for serialization. */
struct ct_mount_plan { const char *backend; const char *strategy; const char *completeness; const char *group_mode; const struct ct_mount_plan_entry *entries; size_t entry_count; };
/** Called for each validated semantic manifest entry in launcher order. */
typedef int (*ct_mount_plan_entry_visitor)(const struct ct_mount_plan_entry *entry, void *context);
/** Stable copied metadata from one validated mount-plan record. */
struct ct_mount_plan_metadata {
  char digest[65];
  char backend[16];
  char strategy[16];
  char completeness[16];
  char group_mode[32];
  size_t entry_count;
};
/** Typed exact-manifest read outcomes used by host doctor diagnostics. */
enum ct_mount_plan_read_status {
  CT_MOUNT_PLAN_READ_OK = 0,
  CT_MOUNT_PLAN_READ_ABSENT = 1,
  CT_MOUNT_PLAN_READ_MALFORMED = 2,
  CT_MOUNT_PLAN_READ_FUTURE = 3,
  CT_MOUNT_PLAN_READ_DIGEST_MISMATCH = 4,
  CT_MOUNT_PLAN_READ_SEMANTIC_INVALID = 5,
  CT_MOUNT_PLAN_READ_CHANGED = 6,
  CT_MOUNT_PLAN_READ_IO = 7
};

/** Validate the frozen ct-mount-plan-v1 semantic grammar and bounds. */
int ct_mount_plan_validate(const struct ct_mount_plan *plan);
/** Serialize a valid plan as canonical NUL-delimited ct-mount-plan-v1 bytes. */
int ct_mount_plan_serialize(const struct ct_mount_plan *plan, unsigned char **bytes, size_t *length, char digest[65]);
/** Parse and validate canonical major-1 plan bytes, including digest and final NUL. */
int ct_mount_plan_parse(const unsigned char *bytes, size_t length);
/**
 * Parse one exact bounded manifest and visit its validated semantic entries.
 *
 * The visitor's strings are valid only during this call. This shared parser
 * keeps selected-root consumers on the frozen producer grammar. `metadata` is
 * copied before visitation and remains valid afterward. A visitor failure
 * aborts without visiting later entries.
 *
 * @param bytes Exact record bytes including the final NUL.
 * @param length Bounded byte length.
 * @param metadata Destination copied record metadata.
 * @param visitor Required entry callback.
 * @param context Opaque callback state.
 * @return Zero only when validation and every visitor call succeed.
 */
int ct_mount_plan_visit(const unsigned char *bytes, size_t length,
                        struct ct_mount_plan_metadata *metadata,
                        ct_mount_plan_entry_visitor visitor, void *context);
/**
 * Read one exact manifest path, detect in-place change or replacement, and visit it.
 *
 * Ordinary symlinks are followed under caller permissions. The opened regular
 * file and the final path target must retain the same size, timestamps, device,
 * and inode through the bounded read. No descriptor remains open on return.
 *
 * @return Zero only for one stable valid record accepted by every visitor.
 */
int ct_mount_plan_read(const char *path, struct ct_mount_plan_metadata *metadata,
                       ct_mount_plan_entry_visitor visitor, void *context);
/** Read one exact manifest and retain its typed failure class. */
enum ct_mount_plan_read_status ct_mount_plan_read_status(
    const char *path, struct ct_mount_plan_metadata *metadata,
    ct_mount_plan_entry_visitor visitor, void *context);
/** Publish one content-addressed plan in state_root and return its immutable path. */
int ct_mount_plan_publish(const struct ct_mount_plan *plan, const char *state_root, char path[4096]);
/** Return nonzero when a writable source would expose private manifest state. */
int ct_mount_plan_source_exposes_state(const char *source, const char *state_root);

#endif
