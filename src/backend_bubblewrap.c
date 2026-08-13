/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_bubblewrap.h"

#include "storage_timeout.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct ct_bubblewrap_root_entries {
  char *names[CT_NESTED_ROOT_ENTRY_LIMIT];
  size_t count;
};

struct ct_bubblewrap_build_context {
  const struct ct_nested_request *request;
  int probe;
  struct ct_nested_command *command;
  const char *root;
};

static int ct_bubblewrap_name_compare(const void *left, const void *right)
{
  const char *const *left_name = left;
  const char *const *right_name = right;
  return strcmp(*left_name, *right_name);
}

static void ct_bubblewrap_root_entries_destroy(
    struct ct_bubblewrap_root_entries *entries)
{
  size_t index;
  for (index = 0U; index < entries->count; ++index) free(entries->names[index]);
  entries->count = 0U;
}

static enum ct_nested_build_result ct_bubblewrap_root_entries_collect(
    const char *root, struct ct_bubblewrap_root_entries *entries)
{
  DIR *directory;
  struct dirent *entry;
  enum ct_nested_build_result result = CT_NESTED_BUILD_OK;
  entries->count = 0U;
  directory = opendir(root);
  if (directory == NULL) return CT_NESTED_BUILD_INTERNAL_FAILURE;
  errno = 0;
  while ((entry = readdir(directory)) != NULL) {
    char *name;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (entries->count == CT_NESTED_ROOT_ENTRY_LIMIT) {
      result = CT_NESTED_BUILD_UNSUPPORTED;
      break;
    }
    name = strdup(entry->d_name);
    if (name == NULL) {
      result = CT_NESTED_BUILD_INTERNAL_FAILURE;
      break;
    }
    entries->names[entries->count++] = name;
    errno = 0;
  }
  if (result == CT_NESTED_BUILD_OK && errno != 0) {
    result = CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  if (closedir(directory) != 0 && result == CT_NESTED_BUILD_OK) {
    result = CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  if (result != CT_NESTED_BUILD_OK) {
    ct_bubblewrap_root_entries_destroy(entries);
    return result;
  }
  qsort(entries->names, entries->count, sizeof(entries->names[0]),
        ct_bubblewrap_name_compare);
  return CT_NESTED_BUILD_OK;
}

static enum ct_nested_build_result ct_bubblewrap_add_entry(
    const struct ct_nested_request *request, const char *source,
    const char *target, struct ct_nested_command *command)
{
  char link_target[CT_HOST_PATH_MAX];
  struct stat status;
  ssize_t link_length;
  if (lstat(source, &status) != 0) {
    return CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  if (S_ISDIR(status.st_mode) || S_ISREG(status.st_mode)) {
    return ct_nested_command_add(
               command,
               strcmp(request->profile->root_access, "read-only") == 0
                   ? "--ro-bind"
                   : "--bind") != 0 ||
                   ct_nested_command_add_pair(command, source, "", "") != 0 ||
                   ct_nested_command_add_pair(command, target, "", "") != 0
               ? CT_NESTED_BUILD_INTERNAL_FAILURE
               : CT_NESTED_BUILD_OK;
  }
  if (!S_ISLNK(status.st_mode)) return CT_NESTED_BUILD_UNSUPPORTED;
  link_length = readlink(source, link_target, sizeof(link_target) - 1U);
  if (link_length < 0 || (size_t)link_length >= sizeof(link_target) - 1U) {
    return CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  link_target[link_length] = '\0';
  return ct_nested_command_add(command, "--symlink") != 0 ||
                 ct_nested_command_add_pair(command, link_target, "", "") !=
                     0 ||
                 ct_nested_command_add_pair(command, target, "", "") != 0
             ? CT_NESTED_BUILD_INTERNAL_FAILURE
             : CT_NESTED_BUILD_OK;
}

static void ct_bubblewrap_target_relation(
    const struct ct_nested_request *request, const char *target,
    int *exact, int *descendant)
{
  size_t index;
  const size_t length = strlen(target);
  *exact = 0;
  *descendant = 0;
  for (index = 0U; index < request->map->count; ++index) {
    const char *mapped = request->map->entries[index].target;
    if (strcmp(mapped, target) == 0) {
      *exact = 1;
    } else if (strcmp(target, "/") == 0 ||
               (strncmp(mapped, target, length) == 0 && mapped[length] == '/')) {
      *descendant = 1;
    }
  }
}

static int ct_bubblewrap_kernel_target(const char *target)
{
  return strcmp(target, "/proc") == 0 ||
         strncmp(target, "/proc/", 6U) == 0 ||
         strcmp(target, "/sys") == 0 ||
         strncmp(target, "/sys/", 5U) == 0 ||
         strcmp(target, "/dev") == 0 ||
         strncmp(target, "/dev/", 5U) == 0;
}

static enum ct_nested_build_result ct_bubblewrap_add_lower_directory(
    const struct ct_nested_request *request, const char *source,
    const char *target, unsigned int depth, size_t *entry_count,
    struct ct_nested_command *command)
{
  struct ct_bubblewrap_root_entries entries;
  size_t index;
  enum ct_nested_build_result collect_result;
  if (depth > 64U) return CT_NESTED_BUILD_UNSUPPORTED;
  collect_result = ct_bubblewrap_root_entries_collect(source, &entries);
  if (collect_result != CT_NESTED_BUILD_OK) return collect_result;
  for (index = 0U; index < entries.count; ++index) {
    char child_source[CT_HOST_PATH_MAX];
    char child_target[CT_HOST_PATH_MAX];
    struct stat status;
    int exact;
    int descendant;
    const int source_length =
        snprintf(child_source, sizeof(child_source), "%s%s%s", source,
                 strcmp(source, "/") == 0 ? "" : "/", entries.names[index]);
    const int target_length =
        snprintf(child_target, sizeof(child_target), "%s%s%s", target,
                 strcmp(target, "/") == 0 ? "" : "/", entries.names[index]);
    if (source_length < 0 || (size_t)source_length >= sizeof(child_source) ||
        target_length < 0 || (size_t)target_length >= sizeof(child_target)) {
      ct_bubblewrap_root_entries_destroy(&entries);
      return CT_NESTED_BUILD_UNSUPPORTED;
    }
    if (++*entry_count > CT_NESTED_ROOT_ENTRY_LIMIT) {
      ct_bubblewrap_root_entries_destroy(&entries);
      return CT_NESTED_BUILD_UNSUPPORTED;
    }
    if (lstat(child_source, &status) != 0) {
      ct_bubblewrap_root_entries_destroy(&entries);
      return CT_NESTED_BUILD_INTERNAL_FAILURE;
    }
    ct_bubblewrap_target_relation(request, child_target, &exact, &descendant);
    if (ct_bubblewrap_kernel_target(child_target) != 0 || exact != 0) continue;
    if (descendant != 0) {
      enum ct_nested_build_result child_result;
      if (!S_ISDIR(status.st_mode)) {
        ct_bubblewrap_root_entries_destroy(&entries);
        return CT_NESTED_BUILD_UNSUPPORTED;
      }
      if (ct_nested_command_add(command, "--dir") != 0 ||
          ct_nested_command_add_pair(command, child_target, "", "") != 0) {
        ct_bubblewrap_root_entries_destroy(&entries);
        return CT_NESTED_BUILD_INTERNAL_FAILURE;
      }
      child_result = ct_bubblewrap_add_lower_directory(
          request, child_source, child_target, depth + 1U, entry_count,
          command);
      if (child_result != CT_NESTED_BUILD_OK) {
        ct_bubblewrap_root_entries_destroy(&entries);
        return child_result;
      }
    } else {
      const enum ct_nested_build_result entry_result = ct_bubblewrap_add_entry(
          request, child_source, child_target, command);
      if (entry_result != CT_NESTED_BUILD_OK) {
        ct_bubblewrap_root_entries_destroy(&entries);
        return entry_result;
      }
    }
  }
  ct_bubblewrap_root_entries_destroy(&entries);
  return CT_NESTED_BUILD_OK;
}

static int ct_bubblewrap_add_overlay_parent(
    const char *target, struct ct_nested_command *command)
{
  char parent[CT_HOST_PATH_MAX];
  char *separator;
  if (ct_host_copy_bounded(parent, sizeof(parent), target) != 0) return 2;
  separator = strrchr(parent, '/');
  if (separator == NULL) return 2;
  if (separator == parent) return 0;
  *separator = '\0';
  return ct_nested_command_add(command, "--dir") != 0 ||
                 ct_nested_command_add_pair(command, parent, "", "") != 0
             ? 1
             : 0;
}

static enum ct_nested_build_result ct_bubblewrap_target(
    const struct ct_nested_request *request, const char *target,
    const char *source, int allow_missing)
{
  char lower[CT_HOST_PATH_MAX];
  struct stat destination;
  struct stat visible;
  if (ct_path_map_root_path(request->profile->root, target, lower) != 0 ||
      stat(source, &visible) != 0) {
    return CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  if (stat(lower, &destination) != 0) {
    return allow_missing != 0 && errno == ENOENT
               ? CT_NESTED_BUILD_OK
               : CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  if (S_ISDIR(destination.st_mode) != S_ISDIR(visible.st_mode)) {
    return CT_NESTED_BUILD_UNSUPPORTED;
  }
  return !S_ISDIR(destination.st_mode) &&
                 (destination.st_mode & 0170000U) != (visible.st_mode & 0170000U)
             ? CT_NESTED_BUILD_UNSUPPORTED
             : CT_NESTED_BUILD_OK;
}

static enum ct_nested_build_result ct_bubblewrap_overlay_target(
    const struct ct_nested_request *request, size_t position)
{
  const struct ct_path_map_entry *entry = &request->map->entries[position];
  const struct ct_path_map_entry *parent = NULL;
  struct stat destination;
  struct stat visible;
  char path[CT_HOST_PATH_MAX];
  size_t index;
  for (index = 0U; index < position; ++index) {
    const struct ct_path_map_entry *candidate = &request->map->entries[index];
    const size_t length = strlen(candidate->target);
    if (strncmp(entry->target, candidate->target, length) == 0 &&
        entry->target[length] == '/' &&
        (parent == NULL || strlen(parent->target) < length)) {
      parent = candidate;
    }
  }
  if (parent == NULL) {
    return ct_bubblewrap_target(request, entry->target, entry->visible, 1);
  }
  if (snprintf(path, sizeof(path), "%s%s", parent->visible,
               entry->target + strlen(parent->target)) >= (int)sizeof(path)) {
    return CT_NESTED_BUILD_UNSUPPORTED;
  }
  if (stat(path, &destination) != 0) {
    return errno == ENOENT ? CT_NESTED_BUILD_UNSUPPORTED
                           : CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  if (stat(entry->visible, &visible) != 0) {
    return CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  if (S_ISDIR(destination.st_mode) != S_ISDIR(visible.st_mode)) {
    return CT_NESTED_BUILD_UNSUPPORTED;
  }
  return !S_ISDIR(destination.st_mode) &&
                 (destination.st_mode & 0170000U) !=
                     (visible.st_mode & 0170000U)
             ? CT_NESTED_BUILD_UNSUPPORTED
             : CT_NESTED_BUILD_OK;
}

static int ct_bubblewrap_add_kernel(const char *option, const char *target,
                                    struct ct_nested_command *command)
{
  return ct_nested_command_add(command, "--dir") != 0 ||
                 ct_nested_command_add(command, target) != 0 ||
                 ct_nested_command_add(command, option) != 0 ||
                 ct_nested_command_add(command, target) != 0 ||
                 ct_nested_command_add(command, target) != 0
             ? 1
             : 0;
}

static int ct_bubblewrap_build(void *opaque)
{
  struct ct_bubblewrap_build_context *context = opaque;
  const struct ct_nested_request *request = context->request;
  struct ct_nested_command *command = context->command;
  enum ct_nested_build_result build_result;
  size_t root_entry_count = 0U;
  size_t index;
  const char *tool;
  build_result =
      ct_bubblewrap_target(request, "/", request->profile->root, 0);
  if (build_result != CT_NESTED_BUILD_OK) return (int)build_result;
  tool = request->bubblewrap_path == NULL ? "bwrap" : request->bubblewrap_path;
  if (ct_nested_command_add(command, tool) != 0 ||
      ct_nested_command_add(command, "--tmpfs") != 0 ||
      ct_nested_command_add(command, "/") != 0) {
    return CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  build_result = ct_bubblewrap_add_lower_directory(
      request, context->root, "/", 0U, &root_entry_count, command);
  if (build_result != CT_NESTED_BUILD_OK) return (int)build_result;
  for (index = 0U; index < request->map->count; ++index) {
    const struct ct_path_map_entry *entry = &request->map->entries[index];
    if (ct_bubblewrap_kernel_target(entry->target) != 0) {
      return CT_NESTED_BUILD_UNSUPPORTED;
    }
    if (ct_bubblewrap_add_overlay_parent(entry->target, command) != 0) {
      return CT_NESTED_BUILD_INTERNAL_FAILURE;
    }
  }
  for (index = 0U; index < request->map->count; ++index) {
    const struct ct_path_map_entry *entry = &request->map->entries[index];
    build_result = ct_bubblewrap_overlay_target(request, index);
    if (build_result != CT_NESTED_BUILD_OK) return (int)build_result;
    if (ct_nested_command_add(
            command, strcmp(entry->access, "read-only") == 0 ? "--ro-bind"
                                                                : "--bind") != 0 ||
        ct_nested_command_add(command, entry->visible) != 0 ||
        ct_nested_command_add(command, entry->target) != 0) {
      return CT_NESTED_BUILD_INTERNAL_FAILURE;
    }
  }
  if (ct_bubblewrap_add_kernel("--ro-bind", "/proc", command) != 0 ||
      ct_bubblewrap_add_kernel("--ro-bind", "/sys", command) != 0 ||
      ct_bubblewrap_add_kernel("--dev-bind", "/dev", command) != 0 ||
      (strcmp(request->profile->root_access, "read-only") == 0 &&
       (ct_nested_command_add(command, "--remount-ro") != 0 ||
        ct_nested_command_add(command, "/") != 0)) ||
       ct_nested_command_add(command, "--chdir") != 0 ||
       ct_nested_command_add(command, request->cwd) != 0 ||
       ct_nested_command_add_trampoline(request, context->probe, command) != 0) {
    return CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  return CT_NESTED_BUILD_OK;
}

enum ct_nested_build_result ct_backend_bubblewrap_arguments(
    const struct ct_nested_request *request, int probe,
    struct ct_nested_command *command)
{
  struct ct_bubblewrap_build_context context;
  int result;
  if (request == NULL || request->profile == NULL || request->map == NULL ||
      request->cwd == NULL || command == NULL || command->arguments == NULL ||
      request->trampoline_descriptor < 0 || request->control_descriptor < 0) {
    return CT_NESTED_BUILD_INTERNAL_FAILURE;
  }
  context.request = request;
  context.probe = probe;
  context.command = command;
  context.root = request->profile->root;
#ifdef CT_SUPERVISOR_TEST_SEAM
  {
    const char *test_root = getenv("CT_TEST_BUBBLEWRAP_ASSEMBLY_ROOT");
    if (test_root != NULL && test_root[0] != '\0') context.root = test_root;
  }
#endif
  result = ct_storage_timeout_call(ct_bubblewrap_build, &context);
  return result >= CT_NESTED_BUILD_OK && result <= CT_NESTED_BUILD_UNSUPPORTED
             ? (enum ct_nested_build_result)result
             : CT_NESTED_BUILD_INTERNAL_FAILURE;
}
