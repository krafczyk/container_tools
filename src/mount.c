/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "mount.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CT_MOUNT_TOKEN_MAX 1024U
#define CT_MOUNT_FILE_MAX 65536U
#define CT_MOUNT_PATH_MAX 4096U

static bool ct_mount_is_flag(const char *token)
{
  return token != NULL && token[0] == '-' && token[1] == '-';
}

static bool ct_mount_append(char *output, size_t output_size, size_t *used, const char *value)
{
  const size_t length = strlen(value);
  if (*used + (output[0] == '\0' ? 0U : 1U) + length >= output_size) return false;
  if (output[0] != '\0') output[(*used)++] = ' ';
  memcpy(output + *used, value, length + 1U);
  *used += length;
  return true;
}

enum ct_mount_status ct_mount_format_args(const char *const *file_tokens,
                                          size_t file_count,
                                          const char *const *extra_tokens,
                                          size_t extra_count, char *output,
                                          size_t output_size)
{
  const char *tokens[CT_MOUNT_TOKEN_MAX];
  size_t groups[CT_MOUNT_TOKEN_MAX];
  size_t count = 0U;
  size_t group_count = 0U;
  size_t current_group = CT_MOUNT_TOKEN_MAX;
  size_t index;
  size_t used = 0U;

  if (output == NULL || output_size == 0U || file_count + extra_count > CT_MOUNT_TOKEN_MAX) {
    return CT_MOUNT_INVALID;
  }
  output[0] = '\0';
  for (index = 0U; index < file_count + extra_count; ++index) {
    const char *token = index < file_count ? file_tokens[index] : extra_tokens[index - file_count];
    if (token == NULL) return CT_MOUNT_INVALID;
    tokens[count] = token;
    if (ct_mount_is_flag(token)) {
      size_t prior;
      for (prior = 0U; prior < count; ++prior) {
        if (ct_mount_is_flag(tokens[prior]) && strcmp(tokens[prior], token) == 0) break;
      }
      if (prior == count) {
        current_group = group_count++;
        groups[count++] = current_group;
      } else {
        current_group = groups[prior];
      }
    } else if (current_group != CT_MOUNT_TOKEN_MAX) {
      groups[count++] = current_group;
    }
  }
  for (index = 0U; index < group_count; ++index) {
    size_t token_index;
    for (token_index = 0U; token_index < count; ++token_index) {
      if (groups[token_index] == index && !ct_mount_append(output, output_size, &used,
                                                            tokens[token_index])) {
        return CT_MOUNT_INVALID;
      }
    }
  }
  return CT_MOUNT_OK;
}

static enum ct_mount_status ct_mount_read_tokens(const char *path, char *contents,
                                                  const char **tokens,
                                                  size_t *count)
{
  FILE *stream;
  size_t bytes;
  char *cursor;
  char *state = NULL;

  if (path == NULL || contents == NULL || tokens == NULL || count == NULL) {
    return CT_MOUNT_INVALID;
  }
  stream = fopen(path, "r");
  if (stream == NULL) return CT_MOUNT_IO;
  bytes = fread(contents, 1U, CT_MOUNT_FILE_MAX, stream);
  if (ferror(stream) != 0 || (!feof(stream) && bytes == CT_MOUNT_FILE_MAX)) {
    (void)fclose(stream);
    return CT_MOUNT_IO;
  }
  (void)fclose(stream);
  contents[bytes] = '\0';
  *count = 0U;
  cursor = strtok_r(contents, " \t\r\n", &state);
  while (cursor != NULL) {
    if (*count == CT_MOUNT_TOKEN_MAX) return CT_MOUNT_INVALID;
    tokens[(*count)++] = cursor;
    cursor = strtok_r(NULL, " \t\r\n", &state);
  }
  return CT_MOUNT_OK;
}

int ct_mount_args_command(int argument_count, char *const arguments[])
{
  char contents[CT_MOUNT_FILE_MAX + 1U];
  const char *file_tokens[CT_MOUNT_TOKEN_MAX];
  char output[CT_MOUNT_FILE_MAX + 1U];
  size_t count;
  enum ct_mount_status status;

  if (argument_count < 1 || arguments == NULL) return 64;
  status = ct_mount_read_tokens(arguments[0], contents, file_tokens, &count);
  if (status != CT_MOUNT_OK ||
      ct_mount_format_args(file_tokens, count, (const char *const *)(arguments + 1),
                           (size_t)(argument_count - 1), output, sizeof(output)) != CT_MOUNT_OK) {
    (void)fprintf(stderr, "container-tools: mount args: invalid configuration\n");
    return 1;
  }
  (void)fprintf(stdout, "%s\n", output);
  return 0;
}

static bool ct_mount_excluded_filesystem(const char *filesystem, const char *const *extra,
                                         size_t extra_count)
{
  static const char *const defaults[] = {"proc", "sysfs", "tmpfs", "devtmpfs", "devpts",
      "securityfs", "cgroup", "pstore", "efivarfs", "debugfs", "tracefs", "configfs",
      "fusectl", "cgroup2", "mqueue", "hugetlbfs"};
  size_t index;
  for (index = 0U; index < sizeof(defaults) / sizeof(defaults[0]); ++index) {
    if (strcmp(filesystem, defaults[index]) == 0) return true;
  }
  for (index = 0U; index < extra_count; ++index) if (strcmp(filesystem, extra[index]) == 0) return true;
  return false;
}

static bool ct_mount_excluded_path(const char *path, const char *const *extra, size_t extra_count)
{
  static const char *const defaults[] = {"/run", "/var", "/sys", "/proc", "/host",
      "/.container-tools-bootstrap", "/.container-tools-instance-identity",
      "/.container-tools-mount-plan"};
  size_t index;
  for (index = 0U; index < sizeof(defaults) / sizeof(defaults[0]) + extra_count; ++index) {
    const char *prefix = index < sizeof(defaults) / sizeof(defaults[0]) ? defaults[index] :
                         extra[index - sizeof(defaults) / sizeof(defaults[0])];
    const size_t length = strlen(prefix);
    if (strncmp(path, prefix, length) == 0 && (path[length] == '\0' || path[length] == '/')) return true;
  }
  return false;
}

static int ct_mount_candidate_compare(const void *left, const void *right)
{
  const char *left_path = left;
  const char *right_path = right;
  const size_t left_length = strlen(left_path);
  const size_t right_length = strlen(right_path);

  if (left_length < right_length) return -1;
  if (left_length > right_length) return 1;
  return strcmp(left_path, right_path);
}

static void ct_mount_sort_candidates(char candidates[][CT_MOUNT_PATH_MAX], size_t candidate_count)
{
  qsort(candidates, candidate_count, sizeof(candidates[0]),
        ct_mount_candidate_compare);
}

int ct_mount_detect_command(int argument_count, char *const arguments[])
{
  const char *extra_filesystems[CT_MOUNT_TOKEN_MAX];
  const char *extra_paths[CT_MOUNT_TOKEN_MAX];
  const char *explicit_paths[CT_MOUNT_TOKEN_MAX];
  char candidates[CT_MOUNT_TOKEN_MAX][CT_MOUNT_PATH_MAX];
  size_t extra_filesystem_count = 0U, extra_path_count = 0U, explicit_count = 0U;
  size_t candidate_count = 0U, index;
  FILE *mounts;
  char line[CT_MOUNT_PATH_MAX * 2U];

  for (index = 0U; index < (size_t)argument_count;) {
    const char *option = arguments[index++];
    int destination = -1;
    if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) {
      (void)fputs("usage: container-tools mount detect [--exclude-fs FS...] [--exclude-path PATH...] [--add-path PATH...]\n", stdout);
      return 0;
    }
    if (strcmp(option, "--exclude-fs") == 0) destination = 0;
    else if (strcmp(option, "--exclude-path") == 0) destination = 1;
    else if (strcmp(option, "--add-path") == 0) destination = 2;
    else return 64;
    while (index < (size_t)argument_count && !ct_mount_is_flag(arguments[index])) {
      if (destination == 0) {
        if (extra_filesystem_count == CT_MOUNT_TOKEN_MAX) return 1;
        extra_filesystems[extra_filesystem_count++] = arguments[index++];
      } else if (destination == 1) {
        if (extra_path_count == CT_MOUNT_TOKEN_MAX) return 1;
        extra_paths[extra_path_count++] = arguments[index++];
      } else {
        if (explicit_count == CT_MOUNT_TOKEN_MAX) return 1;
        explicit_paths[explicit_count++] = arguments[index++];
      }
    }
  }
  mounts = fopen("/proc/mounts", "r");
  if (mounts == NULL) return 1;
  while (fgets(line, sizeof(line), mounts) != NULL) {
    char device[CT_MOUNT_PATH_MAX], path[CT_MOUNT_PATH_MAX], filesystem[256];
    bool duplicate = false;
    if (sscanf(line, "%4095s %4095s %255s", device, path, filesystem) != 3 ||
        strcmp(path, "/") == 0 || path[0] != '/' ||
        ct_mount_excluded_filesystem(filesystem, extra_filesystems, extra_filesystem_count) ||
        ct_mount_excluded_path(path, extra_paths, extra_path_count)) continue;
    for (index = 0U; index < candidate_count; ++index) if (strcmp(candidates[index], path) == 0) duplicate = true;
    if (!duplicate && candidate_count < CT_MOUNT_TOKEN_MAX) {
      (void)snprintf(candidates[candidate_count++], CT_MOUNT_PATH_MAX, "%s", path);
    }
  }
  (void)fclose(mounts);
  ct_mount_sort_candidates(candidates, candidate_count);
  for (index = 0U; index < candidate_count; ++index) {
    size_t parent;
    bool nested = false;
    for (parent = 0U; parent < index; ++parent) {
      const size_t length = strlen(candidates[parent]);
      if (strncmp(candidates[index], candidates[parent], length) == 0 &&
          (candidates[index][length] == '\0' || candidates[index][length] == '/')) nested = true;
    }
    if (!nested) (void)fprintf(stdout, "%s\n", candidates[index]);
  }
  for (index = 0U; index < explicit_count; ++index) (void)fprintf(stdout, "%s\n", explicit_paths[index]);
  return 0;
}
