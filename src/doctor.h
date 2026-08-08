/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_DOCTOR_H
#define CONTAINER_TOOLS_DOCTOR_H

#include "backend_nested.h"

#include <stdio.h>

#define CT_DOCTOR_SCHEMA_VERSION 1U

/** Closed mount-plan diagnosis rendered by `host doctor --json`. */
struct ct_doctor_mount_plan {
  int configured;
  const char *status;
  const char *digest;
  const char *strategy;
  const char *completeness;
  const char *detail;
};

/**
 * Write one complete closed host-doctor JSON object and no surrounding prose.
 *
 * All strings are JSON-escaped by the vendored serializer. The function writes
 * only after the complete object has been built, so allocation or serialization
 * failure cannot expose a partial object.
 *
 * @return Zero after one newline-terminated object, otherwise nonzero.
 */
int ct_doctor_json(
    FILE *stream, const struct ct_host_profile *profile,
    const struct ct_doctor_mount_plan *mount_plan,
    const struct ct_nested_backend_report reports[CT_NESTED_BACKEND_COUNT]);

#endif
