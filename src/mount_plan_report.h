/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_MOUNT_PLAN_REPORT_H
#define CONTAINER_TOOLS_MOUNT_PLAN_REPORT_H

#include "mount_plan.h"

#include <stdio.h>

/** An owned entry retaining one validated mount-plan tuple in source order. */
struct ct_mount_plan_report_entry {
  char *role;
  char *caller_path;
  char *target_path;
  char *access;
  char *recursion;
};

/** An initialized, owned view of one exact validated mount plan. */
struct ct_mount_plan_report {
  struct ct_mount_plan_metadata metadata;
  struct ct_mount_plan_report_entry *entries;
};

/**
 * Initialize an empty report suitable for reading or destruction.
 *
 * @param report Destination report, or NULL for no operation.
 */
void ct_mount_plan_report_init(struct ct_mount_plan_report *report);

/**
 * Release every owned entry in a report, leaving it empty.
 *
 * @param report Initialized report, or NULL for no operation.
 */
void ct_mount_plan_report_destroy(struct ct_mount_plan_report *report);

/**
 * Read one exact mount plan through the production validation parser.
 *
 * @param path Caller-selected manifest path.
 * @param report Initialized destination replaced only after a complete read.
 * @return The parser's typed read result. On success, report owns the new data;
 *         on failure, its prior data is unchanged. Allocation failure is I/O.
 */
enum ct_mount_plan_read_status ct_mount_plan_report_read(
    const char *path, struct ct_mount_plan_report *report);

/**
 * Write one complete `container-tools.mount-plan-inspect/v1` JSON object.
 *
 * Path fields use the schema's lossless backslash-escaped byte encoding.
 * SIGPIPE is ignored only while writing and its prior disposition is restored.
 *
 * @param stream Output stream.
 * @param report Initialized validated report.
 * @return Zero after a flushed newline-terminated object, otherwise nonzero.
 */
int ct_mount_plan_report_write_json(FILE *stream,
                                    const struct ct_mount_plan_report *report);

/**
 * Write a deterministic human-readable validated mount-plan report.
 *
 * Path bytes are safely escaped. SIGPIPE is ignored only while writing and its
 * prior disposition is restored.
 *
 * @param stream Output stream.
 * @param report Initialized validated report.
 * @return Zero after the full flushed report, otherwise nonzero.
 */
int ct_mount_plan_report_write_human(FILE *stream,
                                     const struct ct_mount_plan_report *report);

/**
 * Write one complete `container-tools.mount-plan-compare/v1` JSON object.
 *
 * @param stream Output stream.
 * @param equal Nonzero when the reports have equal canonical content.
 * @param left Initialized left report.
 * @param right Initialized right report.
 * @return Zero after a flushed newline-terminated object, otherwise nonzero.
 */
int ct_mount_plan_report_compare_write_json(
    FILE *stream, int equal, const struct ct_mount_plan_report *left,
    const struct ct_mount_plan_report *right);

/**
 * Write a deterministic human-readable comparison of two validated reports.
 *
 * @param stream Output stream.
 * @param equal Nonzero when the reports have equal canonical content.
 * @param left Initialized left report.
 * @param right Initialized right report.
 * @return Zero after the full flushed comparison, otherwise nonzero.
 */
int ct_mount_plan_report_compare_write_human(
    FILE *stream, int equal, const struct ct_mount_plan_report *left,
    const struct ct_mount_plan_report *right);

#endif
