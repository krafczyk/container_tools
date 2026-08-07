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

/** Validate the frozen ct-mount-plan-v1 semantic grammar and bounds. */
int ct_mount_plan_validate(const struct ct_mount_plan *plan);
/** Serialize a valid plan as canonical NUL-delimited ct-mount-plan-v1 bytes. */
int ct_mount_plan_serialize(const struct ct_mount_plan *plan, unsigned char **bytes, size_t *length, char digest[65]);
/** Parse and validate canonical major-1 plan bytes, including digest and final NUL. */
int ct_mount_plan_parse(const unsigned char *bytes, size_t length);
/** Publish one content-addressed plan in state_root and return its immutable path. */
int ct_mount_plan_publish(const struct ct_mount_plan *plan, const char *state_root, char path[4096]);
/** Return nonzero when a writable source would expose private manifest state. */
int ct_mount_plan_source_exposes_state(const char *source, const char *state_root);

#endif
