/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_INSTANCE_H
#define CONTAINER_TOOLS_INSTANCE_H

/**
 * Run one native persistent-instance operation after the `instance` verb.
 *
 * Parses the backend, canonical instance root, profile-affecting options, image,
 * and payload from `arguments`. Identity mode performs bounded canonical
 * preparation and prints the finalized digest without contacting an instance;
 * execution mode safely creates or reuses the matching instance and returns the
 * payload status. Returns nonzero on invalid input, incomplete preparation,
 * ambiguous liveness, state mismatch, timeout, or runtime failure.
 */
int ct_instance_command(int argument_count, char *const arguments[], int identity_only);

#endif
