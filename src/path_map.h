/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_PATH_MAP_H
#define CONTAINER_TOOLS_PATH_MAP_H

#include "host_config.h"
#include "mount_plan.h"

#include <stddef.h>

#define CT_PATH_MAP_MAX_ENTRIES (CT_HOST_MAX_PROJECTIONS + 4096U)

/** A deterministic selected-root overlay mapping. */
struct ct_path_map_entry { char *visible; char *target; char access[16]; unsigned int source_rank; unsigned int declaration_order; };
/** Complete ordered overlay plan after manifest and profile composition. */
struct ct_path_map {
  struct ct_path_map_entry *entries;
  size_t count;
  size_t capacity;
  char root_visible[CT_HOST_PATH_MAX];
  int root_configured;
};

/** State accumulated while consuming one exact semantic mount plan. */
struct ct_path_map_manifest_context {
  struct ct_path_map *map;
  const struct ct_host_profile *profile;
  unsigned int order;
  size_t generated_roots;
};

/** One explicit planned environment operation. */
enum ct_host_environment_operation_kind {
  CT_HOST_ENVIRONMENT_REMOVE = 0,
  CT_HOST_ENVIRONMENT_SET = 1
};

/** Ordered remove or set operation applied before selected-root dispatch. */
struct ct_host_environment_operation {
  enum ct_host_environment_operation_kind kind;
  char name[128];
  char value[CT_HOST_PATH_MAX];
};

/** Initialize `map` as an empty bounded overlay plan without allocation. */
void ct_path_map_init(struct ct_path_map *map);
/** Release owned entries and reset `map`; a NULL pointer has no effect. */
void ct_path_map_destroy(struct ct_path_map *map);
/**
 * Set the sole normalized lower-root caller path used for cwd mapping.
 *
 * @param map Initialized mutable plan.
 * @param visible Absolute normalized caller-visible root.
 * @return Zero on success or nonzero for invalid input or repeated setup.
 */
int ct_path_map_set_root(struct ct_path_map *map, const char *visible);

/**
 * Add one mapping to a bounded overlay plan.
 *
 * @param map Mutable plan.
 * @param visible Caller-visible source path.
 * @param target Selected-root target path, never `/`.
 * @param access `inherit` or `read-only`.
 * @param source_rank Manifest is zero and configured projections are one.
 * @param declaration_order Stable order inside its source.
 * @return Zero on success or nonzero for invalid, duplicate, or excessive input.
 */
int ct_path_map_add(struct ct_path_map *map, const char *visible, const char *target, const char *access, unsigned int source_rank, unsigned int declaration_order);
/**
 * Sort mappings parent-before-child with stable source/declaration tie breaking.
 *
 * @param map Initialized mutable plan; entries are reordered in place.
 * @return Zero on success or nonzero on allocation or inconsistent-plan failure.
 */
int ct_path_map_sort(struct ct_path_map *map);
/**
 * Map an absolute caller cwd by longest visible-prefix match.
 *
 * Overlays take precedence over the lower root. `unmapped_policy` is `error`
 * or `root`; the destination is written only on success.
 *
 * @return Zero on success or nonzero for malformed or unmapped input.
 */
int ct_path_map_cwd(const struct ct_path_map *map, const char *cwd, const char *unmapped_policy, char target[CT_HOST_PATH_MAX]);
/**
 * Join a normalized caller-visible root and absolute target-side path.
 *
 * @return Zero with `result` populated, or nonzero for malformed/oversized input.
 */
int ct_path_map_root_path(const char *root, const char *target, char result[CT_HOST_PATH_MAX]);
/**
 * Resolve one target-side path through the longest composed overlay.
 *
 * @param map Complete selected-root path map.
 * @param target Normalized absolute target-side path.
 * @param visible Destination caller-visible path.
 * @return Zero on success or nonzero for malformed, unmapped, or oversized input.
 */
int ct_path_map_visible(const struct ct_path_map *map, const char *target,
                        char visible[CT_HOST_PATH_MAX]);
/**
 * Build ordered remove, PATH, and set operations without mutating `environ`.
 *
 * @param profile Valid strict profile.
 * @param output Caller-owned bounded operation array.
 * @param output_count Receives the number of complete operations.
 * @return Zero on success or nonzero without changing process environment.
 */
int ct_path_map_environment(const struct ct_host_profile *profile, struct ct_host_environment_operation output[CT_HOST_MAX_ENVIRONMENT + 1U], size_t *output_count);
/** Initialize a no-allocation shared-parser visitor for one profile and map. */
void ct_path_map_manifest_init(struct ct_path_map_manifest_context *context,
                               struct ct_path_map *map,
                               const struct ct_host_profile *profile);
/**
 * Validate and compose one borrowed shared-parser semantic manifest entry.
 *
 * Bootstrap/generated entries are validated but not replayed. Caller-data
 * entries are appended to the map and exact target collisions fail.
 *
 * @return Zero on success or nonzero for semantic/profile inconsistency.
 */
int ct_path_map_manifest_entry(const struct ct_mount_plan_entry *entry,
                               void *context);
/**
 * Enforce full-root strategy, completeness, and generated-root evidence.
 *
 * Rewrite profiles do not require full-root evidence. The function is
 * read-only and returns nonzero when a full-root plan is ineligible.
 */
int ct_path_map_manifest_eligible(
    const struct ct_host_profile *profile,
    const struct ct_mount_plan_metadata *metadata,
    const struct ct_path_map_manifest_context *context);

#endif
