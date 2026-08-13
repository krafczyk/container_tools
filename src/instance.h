/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_INSTANCE_H
#define CONTAINER_TOOLS_INSTANCE_H

/** Outcomes of resolving a shortened managed runtime instance name. */
enum ct_instance_runtime_selector_status {
  CT_INSTANCE_RUNTIME_SELECTOR_OK = 0,
  CT_INSTANCE_RUNTIME_SELECTOR_ABSENT = 1,
  CT_INSTANCE_RUNTIME_SELECTOR_AMBIGUOUS = 2,
  CT_INSTANCE_RUNTIME_SELECTOR_IO = 3,
  CT_INSTANCE_RUNTIME_SELECTOR_INTERRUPTED = 130,
  CT_INSTANCE_RUNTIME_SELECTOR_TERMINATED = 143,
};

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
/**
 * Resolve one active managed instance whose 32-hex name begins with a prefix.
 *
 * @param backend Selected `apptainer` or `singularity` executable.
 * @param runtime_argument Optional opaque runtime argument.
 * @param prefix Nonempty lowercase hexadecimal prefix no longer than 32 bytes.
 * @param name Receives the exact managed instance name when unique.
 * @return A selector outcome, except interrupt and termination signals remain
 * encoded as process statuses.
 */
enum ct_instance_runtime_selector_status ct_instance_resolve_runtime_prefix(
    const char *backend, const char *runtime_argument, const char *prefix,
    char name[40]);

#endif
