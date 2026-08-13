/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_INSTANCE_PROFILE_REPORT_H
#define CONTAINER_TOOLS_INSTANCE_PROFILE_REPORT_H

#include "instance_profile_manifest.h"
#include <stdio.h>

/**
 * Write a complete JSON profile report using lossless backslash-escaped path bytes.
 *
 * @param stream Destination stream.
 * @param manifest Valid parsed profile manifest.
 * @return Zero after one flushed complete object; nonzero on report failure.
 */
int ct_instance_profile_report_write_json(FILE *stream, const struct ct_instance_profile_manifest *manifest);
/**
 * Write a deterministic human-readable profile report using safely escaped path bytes.
 *
 * @param stream Destination stream.
 * @param manifest Valid parsed profile manifest.
 * @return Zero after a flushed complete report; nonzero on report failure.
 */
int ct_instance_profile_report_write_human(FILE *stream, const struct ct_instance_profile_manifest *manifest);

#endif
