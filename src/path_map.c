/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "path_map.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void ct_path_map_init(struct ct_path_map *map)
{
  if (map != NULL) {
    map->entries = NULL;
    map->count = 0U;
    map->capacity = 0U;
    map->root_visible[0] = '\0';
    map->root_configured = 0;
  }
}

int ct_path_map_set_root(struct ct_path_map *map, const char *visible)
{
  if (map == NULL || map->root_configured != 0 ||
      ct_host_path_validate(visible) != 0 ||
      ct_host_copy_bounded(map->root_visible, sizeof(map->root_visible), visible) != 0) return 1;
  map->root_configured = 1;
  return 0;
}

void ct_path_map_destroy(struct ct_path_map *map)
{
  if (map != NULL) {
    size_t index;
    for (index = 0U; index < map->count; ++index) {
      free(map->entries[index].visible);
      free(map->entries[index].target);
    }
    free(map->entries);
    ct_path_map_init(map);
  }
}

int ct_path_map_add(struct ct_path_map *map, const char *visible, const char *target, const char *access, unsigned int source_rank, unsigned int declaration_order)
{
  size_t index;
  char *visible_copy, *target_copy;
  if (map == NULL || map->count >= CT_PATH_MAP_MAX_ENTRIES ||
      ct_host_path_validate(visible) != 0 || ct_host_path_validate(target) != 0 ||
      strcmp(target, "/") == 0 ||
      (strcmp(access, "inherit") != 0 && strcmp(access, "read-only") != 0)) return 1;
  for (index = 0U; index < map->count; ++index) if (strcmp(map->entries[index].target, target) == 0) return 1;
  if (map->count == map->capacity) {
    size_t capacity = map->capacity == 0U ? 16U : map->capacity * 2U;
    struct ct_path_map_entry *entries;
    if (capacity < map->capacity) return 1;
    if (capacity > CT_PATH_MAP_MAX_ENTRIES) capacity = CT_PATH_MAP_MAX_ENTRIES;
    entries = realloc(map->entries, capacity * sizeof(*entries));
    if (entries == NULL) return 1;
    map->entries = entries; map->capacity = capacity;
  }
  visible_copy = strdup(visible);
  target_copy = strdup(target);
  if (visible_copy == NULL || target_copy == NULL ||
      ct_host_copy_bounded(map->entries[map->count].access,
                           sizeof(map->entries[map->count].access), access) != 0) {
    free(visible_copy);
    free(target_copy);
    return 1;
  }
  map->entries[map->count].visible = visible_copy;
  map->entries[map->count].target = target_copy;
  map->entries[map->count].source_rank = source_rank; map->entries[map->count].declaration_order = declaration_order; ++map->count;
  return 0;
}

static int ct_path_parent(const char *parent, const char *child)
{
  const size_t length = strlen(parent);
  return length < strlen(child) && strncmp(parent, child, length) == 0 && child[length] == '/';
}

static int ct_path_precedes(const struct ct_path_map_entry *left,
                            const struct ct_path_map_entry *right)
{
  if (left->source_rank != right->source_rank) {
    return left->source_rank < right->source_rank;
  }
  if (left->declaration_order != right->declaration_order) {
    return left->declaration_order < right->declaration_order;
  }
  return strcmp(left->target, right->target) < 0;
}

int ct_path_map_sort(struct ct_path_map *map)
{
  struct ct_path_map_entry *sorted;
  size_t *ancestors;
  bool *used;
  size_t position;
  if (map == NULL) return 1;
  if (map->count == 0U) return 0;
  sorted = calloc(map->count, sizeof(*sorted));
  ancestors = calloc(map->count, sizeof(*ancestors));
  used = calloc(map->count, sizeof(*used));
  if (sorted == NULL || ancestors == NULL || used == NULL) {
    free(sorted);
    free(ancestors);
    free(used);
    return 1;
  }
  for (position = 0U; position < map->count; ++position) {
    size_t parent;
    for (parent = 0U; parent < map->count; ++parent) {
      if (parent != position &&
          ct_path_parent(map->entries[parent].target,
                         map->entries[position].target) != 0) {
        ++ancestors[position];
      }
    }
  }
  for (position = 0U; position < map->count; ++position) {
    size_t index, selected = map->count;
    for (index = 0U; index < map->count; ++index) {
      if (!used[index] && ancestors[index] == 0U &&
          (selected == map->count ||
           ct_path_precedes(&map->entries[index],
                            &map->entries[selected]) != 0)) {
        selected = index;
      }
    }
    if (selected == map->count) {
      free(sorted);
      free(ancestors);
      free(used);
      return 1;
    }
    sorted[position] = map->entries[selected];
    used[selected] = true;
    for (index = 0U; index < map->count; ++index) {
      if (!used[index] &&
          ct_path_parent(map->entries[selected].target,
                         map->entries[index].target) != 0) {
        --ancestors[index];
      }
    }
  }
  memcpy(map->entries, sorted, map->count * sizeof(*sorted));
  free(sorted);
  free(ancestors);
  free(used);
  return 0;
}

static int ct_path_matches(const char *path, const char *prefix)
{
  const size_t length = strlen(prefix);
  return strcmp(path, prefix) == 0 || strcmp(prefix, "/") == 0 ||
         (strncmp(path, prefix, length) == 0 && path[length] == '/');
}

int ct_path_map_cwd(const struct ct_path_map *map, const char *cwd, const char *unmapped_policy, char target[CT_HOST_PATH_MAX])
{
  size_t index, selected = CT_PATH_MAP_MAX_ENTRIES, selected_length = 0U;
  if (map == NULL || target == NULL || ct_host_path_validate(cwd) != 0 ||
      unmapped_policy == NULL) return 1;
  for (index = 0U; index < map->count; ++index) {
    const size_t length = strlen(map->entries[index].visible);
    if (ct_path_matches(cwd, map->entries[index].visible) != 0 &&
        length > selected_length) {
      selected = index;
      selected_length = length;
    }
  }
  if (selected != CT_PATH_MAP_MAX_ENTRIES) {
    const char *suffix = strcmp(map->entries[selected].visible, "/") == 0
                             ? cwd
                             : cwd + selected_length;
    if (snprintf(target, CT_HOST_PATH_MAX, "%s%s",
                 map->entries[selected].target, suffix) >=
        (int)CT_HOST_PATH_MAX) return 1;
    return 0;
  }
  if (map->root_configured != 0 &&
      ct_path_matches(cwd, map->root_visible) != 0) {
    const char *suffix = strcmp(map->root_visible, "/") == 0
                             ? cwd
                             : cwd + strlen(map->root_visible);
    return suffix[0] == '\0' ? ct_host_copy_bounded(target, CT_HOST_PATH_MAX, "/")
                              : ct_host_copy_bounded(target, CT_HOST_PATH_MAX, suffix);
  }
  if (strcmp(unmapped_policy, "root") != 0) return 1;
  return ct_host_copy_bounded(target, CT_HOST_PATH_MAX, "/");
}

int ct_path_map_root_path(const char *root, const char *target, char result[CT_HOST_PATH_MAX])
{
  if (ct_host_path_validate(root) != 0 || ct_host_path_validate(target) != 0 || result == NULL) return 1;
  if (strcmp(root, "/") == 0) return ct_host_copy_bounded(result, CT_HOST_PATH_MAX, target);
  if (strcmp(target, "/") == 0) return ct_host_copy_bounded(result, CT_HOST_PATH_MAX, root);
  if (snprintf(result, CT_HOST_PATH_MAX, "%s%s", root, target) >= (int)CT_HOST_PATH_MAX) return 1;
  return 0;
}

int ct_path_map_visible(const struct ct_path_map *map, const char *target,
                        char visible[CT_HOST_PATH_MAX])
{
  size_t index, selected = CT_PATH_MAP_MAX_ENTRIES, selected_length = 0U;
  if (map == NULL || visible == NULL || map->root_configured == 0 ||
      ct_host_path_validate(target) != 0) return 1;
  for (index = 0U; index < map->count; ++index) {
    const size_t length = strlen(map->entries[index].target);
    if (ct_path_matches(target, map->entries[index].target) != 0 &&
        length > selected_length) {
      selected = index;
      selected_length = length;
    }
  }
  if (selected != CT_PATH_MAP_MAX_ENTRIES) {
    const char *suffix = target + selected_length;
    return suffix[0] == '\0'
               ? ct_host_copy_bounded(visible, CT_HOST_PATH_MAX,
                                      map->entries[selected].visible)
               : ct_path_map_root_path(map->entries[selected].visible, suffix,
                                       visible);
  }
  return ct_path_map_root_path(map->root_visible, target, visible);
}

int ct_path_map_environment(const struct ct_host_profile *profile, struct ct_host_environment_operation output[CT_HOST_MAX_ENVIRONMENT + 1U], size_t *output_count)
{
  size_t index, length = 0U;
  if (profile == NULL || output == NULL || output_count == NULL) return 1;
  for (index = 0U; index < profile->environment_remove_count; ++index) { if (length >= CT_HOST_MAX_ENVIRONMENT + 1U || ct_host_copy_bounded(output[length].name, sizeof(output[length].name), profile->environment_remove[index]) != 0) return 1; output[length].kind = CT_HOST_ENVIRONMENT_REMOVE; output[length].value[0] = '\0'; ++length; }
  if (length >= CT_HOST_MAX_ENVIRONMENT + 1U || ct_host_copy_bounded(output[length].name, sizeof(output[length].name), "PATH") != 0) return 1;
  output[length].kind = CT_HOST_ENVIRONMENT_SET;
  output[length].value[0] = '\0';
  for (index = 0U; index < profile->path_count; ++index) {
    const size_t entry_length = strlen(profile->path[index]);
    size_t used = strlen(output[length].value);
    const size_t separator = index == 0U ? 0U : 1U;
    if (used + separator + entry_length + 1U > sizeof(output[length].value)) return 1;
    if (separator != 0U) output[length].value[used++] = ':';
    memcpy(output[length].value + used, profile->path[index], entry_length + 1U);
  }
  ++length;
  for (index = 0U; index < profile->environment_count; ++index) { if (length >= CT_HOST_MAX_ENVIRONMENT + 1U || ct_host_copy_bounded(output[length].name, sizeof(output[length].name), profile->environment[index].name) != 0 || ct_host_copy_bounded(output[length].value, sizeof(output[length].value), profile->environment[index].value) != 0) return 1; output[length].kind = CT_HOST_ENVIRONMENT_SET; ++length; }
  *output_count = length;
  return 0;
}

void ct_path_map_manifest_init(struct ct_path_map_manifest_context *context,
                               struct ct_path_map *map,
                               const struct ct_host_profile *profile)
{
  if (context != NULL) {
    context->map = map;
    context->profile = profile;
    context->order = 0U;
    context->generated_roots = 0U;
  }
}

int ct_path_map_manifest_entry(const struct ct_mount_plan_entry *entry,
                               void *opaque)
{
  struct ct_path_map_manifest_context *context = opaque;
  if (entry == NULL || context == NULL || context->map == NULL ||
      context->profile == NULL) return 1;
  if (strcmp(entry->role, "bootstrap-internal") == 0) return 0;
  if (strcmp(entry->role, "generated-host-root") == 0) {
    struct stat caller_status, target_status;
    char expected[CT_HOST_PATH_MAX];
    const char *generated_root = "/host";
#ifdef CT_STORAGE_TIMEOUT_TEST_SEAM
    const char *test_root = getenv("CT_TEST_PATH_MAP_HOST_ROOT");
    if (test_root != NULL && test_root[0] != '\0') generated_root = test_root;
#endif
    if (ct_path_map_root_path(generated_root, entry->target_path, expected) != 0 ||
        (strcmp(entry->caller_path, expected) != 0 &&
         (stat(entry->caller_path, &caller_status) != 0 ||
          stat(expected, &target_status) != 0 ||
          caller_status.st_dev != target_status.st_dev ||
          caller_status.st_ino != target_status.st_ino))) return 1;
    ++context->generated_roots;
    return 0;
  }
  if (strcmp(entry->role, "detected-automatic") != 0 &&
      strcmp(entry->role, "explicit") != 0 &&
      strcmp(entry->role, "persistent-automatic-cwd") != 0) return 1;
  return ct_path_map_add(context->map, entry->caller_path, entry->target_path,
                         entry->access, 0U, context->order++);
}

int ct_path_map_manifest_eligible(
    const struct ct_host_profile *profile,
    const struct ct_mount_plan_metadata *metadata,
    const struct ct_path_map_manifest_context *context)
{
  if (profile == NULL || metadata == NULL || context == NULL) return 1;
  if (strcmp(profile->semantics, "full-root") != 0) return 0;
  return strcmp(metadata->completeness, "complete") != 0 ||
                 (strcmp(metadata->strategy, "direct") != 0 &&
                  strcmp(metadata->strategy, "fallback") != 0) ||
                 context->generated_roots == 0U
             ? 1
             : 0;
}
