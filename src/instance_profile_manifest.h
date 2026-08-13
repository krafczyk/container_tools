/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_INSTANCE_PROFILE_MANIFEST_H
#define CONTAINER_TOOLS_INSTANCE_PROFILE_MANIFEST_H

#include <stddef.h>

#define CT_INSTANCE_PROFILE_MANIFEST_MAX_MOUNTS 4096U
#define CT_INSTANCE_PROFILE_MANIFEST_MAX_GROUPS 1024U
#define CT_INSTANCE_PROFILE_MANIFEST_MAX_BYTES 1048576U
#define CT_INSTANCE_PROFILE_MANIFEST_CACHE_MAX_ENTRIES 4096U

/** One finalized persistent-instance mount identity in creation order. */
struct ct_instance_profile_manifest_mount {
  const char *flag;
  const char *descriptor;
  const char *role;
  const char *access;
  const char *recursion;
  int generated;
  int semantic;
  const char *resolved_source;
  const char *source_identity;
};

/** Complete ct-instance-profile-v1 record with borrowed serialization values. */
struct ct_instance_profile_manifest {
  char record_digest[65];
  char profile_digest[65];
  const char *instance_name;
  const char *backend;
  const char *profile_grammar;
  const char *runtime_argument_digest;
  const char *uid;
  const char *gid;
  const char *hostname;
  const char *home;
  const char *instance_root;
  const char *image_path;
  const char *image_identity;
  const char *bootstrap_path;
  const char *bootstrap_identity;
  const char *projection_grammar;
  const char *projection_digest;
  const char *group_mode;
  const char **groups;
  size_t group_count;
  const char *mount_plan_digest;
  struct ct_instance_profile_manifest_mount *mounts;
  size_t mount_count;
};

/** Typed exact-record read outcomes used by profile inspection. */
enum ct_instance_profile_manifest_read_status {
  CT_INSTANCE_PROFILE_MANIFEST_READ_OK = 0,
  CT_INSTANCE_PROFILE_MANIFEST_READ_ABSENT,
  CT_INSTANCE_PROFILE_MANIFEST_READ_MALFORMED,
  CT_INSTANCE_PROFILE_MANIFEST_READ_FUTURE,
  CT_INSTANCE_PROFILE_MANIFEST_READ_DIGEST_MISMATCH,
  CT_INSTANCE_PROFILE_MANIFEST_READ_SEMANTIC_INVALID,
  CT_INSTANCE_PROFILE_MANIFEST_READ_CHANGED,
  CT_INSTANCE_PROFILE_MANIFEST_READ_IO
};
/** Outcome of resolving a lowercase profile digest prefix in managed instance state. */
enum ct_instance_profile_manifest_selector_status {
  CT_INSTANCE_PROFILE_MANIFEST_SELECTOR_OK = 0,
  CT_INSTANCE_PROFILE_MANIFEST_SELECTOR_ABSENT,
  CT_INSTANCE_PROFILE_MANIFEST_SELECTOR_AMBIGUOUS,
  CT_INSTANCE_PROFILE_MANIFEST_SELECTOR_IO
};

/**
 * Validate ct-instance-profile-v1 scalar grammar, ordering, and bounds.
 *
 * @param manifest Borrowed manifest fields to validate.
 * @return Zero for a complete valid manifest; nonzero for malformed input.
 */
int ct_instance_profile_manifest_validate(const struct ct_instance_profile_manifest *manifest);
/**
 * Recompute the lifecycle profile digest represented by one manifest.
 *
 * @param manifest Manifest fields other than profile_digest and instance_name.
 * @param digest Receives the lowercase SHA-256 lifecycle digest.
 * @return Zero when the represented v4 field sequence is complete and bounded.
 */
int ct_instance_profile_manifest_profile_digest(
    const struct ct_instance_profile_manifest *manifest, char digest[65]);
/**
 * Serialize one valid manifest as canonical NUL-delimited bytes with its internal digest.
 *
 * @param manifest Valid borrowed manifest fields.
 * @param bytes Receives heap-owned bytes released with free.
 * @param length Receives the exact byte count including the final NUL.
 * @return Zero on success; nonzero for invalid input or allocation failure.
 */
int ct_instance_profile_manifest_serialize(const struct ct_instance_profile_manifest *manifest,
                                           unsigned char **bytes, size_t *length);
/**
 * Parse and validate canonical bytes, including the internal record digest.
 *
 * @param bytes Exact bounded record bytes.
 * @param length Exact byte count including the final NUL.
 * @return Zero only for a supported valid record.
 */
int ct_instance_profile_manifest_parse(const unsigned char *bytes, size_t length);
/**
 * Parse canonical bytes and retain their complete validated manifest values.
 *
 * @param bytes Exact bounded record bytes including final NUL.
 * @param length Exact byte count.
 * @param manifest Receives owned fields released with destroy.
 * @return A typed validation status.
 */
enum ct_instance_profile_manifest_read_status ct_instance_profile_manifest_parse_read(
    const unsigned char *bytes, size_t length,
    struct ct_instance_profile_manifest *manifest);
/**
 * Normalize a managed absolute instance-root scalar without accessing or creating it.
 *
 * @param value Raw absolute root with no colon, comma, or newline.
 * @param output Receives the normalized root.
 * @return Zero on success; nonzero for unsafe or oversized input.
 */
int ct_instance_profile_manifest_normalize_root(const char *value, char output[4096]);
/**
 * Read and retain one complete validated manifest from an existing path.
 *
 * @param path Existing manifest path.
 * @param manifest Receives owned fields released with destroy.
 * @return A typed read, validation, future-version, or I/O status.
 */
enum ct_instance_profile_manifest_read_status ct_instance_profile_manifest_read(
    const char *path, struct ct_instance_profile_manifest *manifest);
/**
 * Publish one profile record under ROOT/profiles/DIGEST.manifest atomically.
 *
 * The profile root and profiles directory must be real directories, and the
 * final record must be a regular file rather than a symlink. Existing content
 * is accepted only when byte-identical and valid; competing identical
 * publishers therefore succeed without adopting conflicting state. New
 * directories and records request modes 0700 and 0600 respectively; reported
 * ownership and permission bits are not admission criteria for managed state.
 *
 * @param root Canonical managed instance root.
 * @param manifest Complete validated profile manifest.
 * @param path Receives the immutable managed manifest path.
 * @return Zero on publication or identical reuse, otherwise nonzero.
 * @sideeffect May create ROOT, ROOT/profiles, and one manifest.
 */
int ct_instance_profile_manifest_publish(
    const char *root, const struct ct_instance_profile_manifest *manifest,
    char path[4096]);
/**
 * Read one content-addressed profile manifest from an instance root.
 *
 * @param root Canonical managed instance root.
 * @param digest Exact full profile digest selecting the record.
 * @param manifest Receives owned fields released with destroy.
 * @return A typed state, stability, or record-validation status.
 */
enum ct_instance_profile_manifest_read_status ct_instance_profile_manifest_read_private(
    const char *root, const char *digest,
    struct ct_instance_profile_manifest *manifest);
/**
 * Resolve exactly one profile manifest digest beginning with a prefix.
 *
 * @param root Canonical managed instance root.
 * @param prefix Nonempty lowercase hexadecimal prefix no longer than 64 bytes.
 * @param digest Receives the matched full 64-hex profile digest when unique.
 * @return A selector outcome; no path is returned for absent or ambiguous input.
 */
enum ct_instance_profile_manifest_selector_status
ct_instance_profile_manifest_resolve_private_prefix(const char *root,
                                                    const char *prefix,
                                                    char digest[65]);
/**
 * Release every allocation made by ct_instance_profile_manifest_read.
 *
 * @param manifest Owned or zero-initialized manifest to reset.
 */
void ct_instance_profile_manifest_destroy(struct ct_instance_profile_manifest *manifest);

#endif
