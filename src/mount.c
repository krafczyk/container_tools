/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "mount.h"
#include "cli.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CT_MOUNT_TOKEN_MAX CT_MOUNT_MAX_PATHS
#define CT_MOUNT_FILE_MAX 65536U

static bool ct_mount_is_flag(const char *token)
{
  return token != NULL && token[0] == '-' && token[1] == '-';
}

static bool ct_mount_is_persistent_path(const char *path)
{
  return path != NULL && path[0] == '/' && strpbrk(path, ":,\n") == NULL;
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
  if (stream == NULL) return errno == ENOENT ? CT_MOUNT_NOT_FOUND : CT_MOUNT_IO;
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

static enum ct_mount_status ct_mount_tokenize(char *contents, const char **tokens,
                                               size_t *count)
{
  char *cursor;
  char *state = NULL;

  if (contents == NULL || tokens == NULL || count == NULL) return CT_MOUNT_INVALID;
  *count = 0U;
  cursor = strtok_r(contents, " \t\r\n", &state);
  while (cursor != NULL) {
    if (*count == CT_MOUNT_TOKEN_MAX) return CT_MOUNT_INVALID;
    tokens[(*count)++] = cursor;
    cursor = strtok_r(NULL, " \t\r\n", &state);
  }
  return CT_MOUNT_OK;
}

static enum ct_mount_environment_status ct_mount_validate_environment_options(
    const char *const *tokens, size_t count,
    enum ct_mount_environment_status options_status,
    enum ct_mount_environment_status add_path_status)
{
  bool add_path = false;
  size_t index;
  for (index = 0U; index < count; ++index) {
    const char *token = tokens[index];
    if (ct_mount_is_flag(token)) {
      if (strcmp(token, "--exclude-fs") != 0 &&
          strcmp(token, "--exclude-path") != 0 &&
          strcmp(token, "--add-path") != 0) return options_status;
      add_path = strcmp(token, "--add-path") == 0;
    } else if (add_path && !ct_mount_is_persistent_path(token)) {
      return add_path_status;
    }
  }
  return CT_MOUNT_ENVIRONMENT_OK;
}

int ct_mount_args_command(int argument_count, char *const arguments[])
{
  char contents[CT_MOUNT_FILE_MAX + 1U];
  const char *file_tokens[CT_MOUNT_TOKEN_MAX];
  char output[CT_MOUNT_FILE_MAX + 1U];
  size_t count;
  enum ct_mount_status status;

  if (argument_count < 1 || arguments == NULL) {
    ct_cli_diagnostic("mount args", "usage",
                      "use 'container-tools mount args CONFIG [OPTIONS...]' for valid syntax");
    return 64;
  }
  status = ct_mount_read_tokens(arguments[0], contents, file_tokens, &count);
  if (status != CT_MOUNT_OK) {
    ct_cli_diagnostic("mount args", "configuration",
                      "create a readable whitespace-tokenized configuration file or pass a valid path, then retry");
    return 1;
  }
  if (ct_mount_format_args(file_tokens, count, (const char *const *)(arguments + 1),
                           (size_t)(argument_count - 1), output, sizeof(output)) != CT_MOUNT_OK) {
    ct_cli_diagnostic("mount args", "configuration",
                      "simplify the mount options and keep the configuration within supported limits, then retry");
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

static int ct_mount_collect(int argument_count, char *const arguments[],
                            char output[CT_MOUNT_MAX_PATHS][CT_MOUNT_PATH_MAX],
                            size_t *output_count)
{
  const char *extra_filesystems[CT_MOUNT_TOKEN_MAX];
  const char *extra_paths[CT_MOUNT_TOKEN_MAX];
  const char *explicit_paths[CT_MOUNT_TOKEN_MAX];
  size_t extra_filesystem_count = 0U, extra_path_count = 0U, explicit_count = 0U;
  size_t candidate_count = 0U, index;
  FILE *mounts;
  char line[CT_MOUNT_PATH_MAX * 2U];

  if (output == NULL || output_count == NULL) return 1;
  *output_count = 0U;
  for (index = 0U; index < (size_t)argument_count;) {
    const char *option = arguments[index++];
    int destination = -1;
    if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) {
      return 2;
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
    for (index = 0U; index < candidate_count; ++index) if (strcmp(output[index], path) == 0) duplicate = true;
    if (!duplicate && candidate_count < CT_MOUNT_TOKEN_MAX) {
      (void)snprintf(output[candidate_count++], CT_MOUNT_PATH_MAX, "%s", path);
    }
  }
  (void)fclose(mounts);
  ct_mount_sort_candidates(output, candidate_count);
  for (index = 0U; index < candidate_count; ++index) {
    size_t parent;
    bool nested = false;
    for (parent = 0U; parent < *output_count; ++parent) {
      const size_t length = strlen(output[parent]);
      if (strncmp(output[index], output[parent], length) == 0 &&
          (output[index][length] == '\0' || output[index][length] == '/')) nested = true;
    }
    if (!nested) {
      if (*output_count == CT_MOUNT_MAX_PATHS) return 1;
      if (*output_count != index) {
        memmove(output[*output_count], output[index], CT_MOUNT_PATH_MAX);
      }
      ++*output_count;
    }
  }
  for (index = 0U; index < explicit_count; ++index) {
    size_t existing;
    for (existing = 0U; existing < *output_count; ++existing) {
      if (strcmp(output[existing], explicit_paths[index]) == 0) break;
    }
    if (existing != *output_count) continue;
    if (*output_count == CT_MOUNT_MAX_PATHS ||
        snprintf(output[(*output_count)++], CT_MOUNT_PATH_MAX, "%s",
                 explicit_paths[index]) >= (int)CT_MOUNT_PATH_MAX) return 1;
  }
  return 0;
}

int ct_mount_detect_command(int argument_count, char *const arguments[])
{
  char paths[CT_MOUNT_MAX_PATHS][CT_MOUNT_PATH_MAX];
  size_t count = 0U;
  size_t index;
  const int result = ct_mount_collect(argument_count, arguments, paths, &count);

  if (result == 2) {
    (void)fputs("usage: container-tools mount detect [--exclude-fs FS...] [--exclude-path PATH...] [--add-path PATH...]\n", stdout);
    return 0;
  }
  if (result == 64) {
    ct_cli_diagnostic("mount detect", "usage",
                      "use 'container-tools mount detect --help' for valid syntax");
    return 64;
  }
  if (result != 0) {
    ct_cli_diagnostic("mount detect", "discovery",
                      "fix the mount options or /proc/mounts access, then retry");
    return result;
  }
  for (index = 0U; index < count; ++index) (void)fprintf(stdout, "%s\n", paths[index]);
  return 0;
}

enum ct_mount_environment_status ct_mount_collect_environment(
    char paths[CT_MOUNT_MAX_PATHS][CT_MOUNT_PATH_MAX], size_t *path_count)
{
  char config_path[CT_MOUNT_PATH_MAX];
  char file_contents[CT_MOUNT_FILE_MAX + 1U] = "";
  char extra_contents[CT_MOUNT_FILE_MAX + 1U] = "";
  char formatted[CT_MOUNT_FILE_MAX + 1U];
  const char *file_tokens[CT_MOUNT_TOKEN_MAX];
  const char *extra_tokens[CT_MOUNT_TOKEN_MAX];
  const char *formatted_tokens[CT_MOUNT_TOKEN_MAX];
  size_t file_count = 0U, extra_count = 0U, formatted_count = 0U;
  size_t index;
  const char *configured = getenv("CT_MOUNT_CFG");
  const char *extra = getenv("MOUNT_DETECTOR_ARGS");
  const char *home = getenv("HOME");
  enum ct_mount_status status;
  enum ct_mount_environment_status environment_status;

  if (paths == NULL || path_count == NULL) return CT_MOUNT_ENVIRONMENT_INTERNAL;
  *path_count = 0U;
  if (configured == NULL || configured[0] == '\0') {
    if (home == NULL || home[0] != '/' ||
        snprintf(config_path, sizeof(config_path), "%s/.config/ct_mount.conf", home) >=
            (int)sizeof(config_path)) return CT_MOUNT_ENVIRONMENT_CONFIG_OPTIONS;
    configured = config_path;
  }
  status = ct_mount_read_tokens(configured, file_contents, file_tokens, &file_count);
  if (status == CT_MOUNT_NOT_FOUND) {
    file_count = 0U;
  } else if (status == CT_MOUNT_IO) {
    return CT_MOUNT_ENVIRONMENT_CONFIG_IO;
  } else if (status != CT_MOUNT_OK) {
    return CT_MOUNT_ENVIRONMENT_CONFIG_OPTIONS;
  }
  environment_status = ct_mount_validate_environment_options(
      file_tokens, file_count, CT_MOUNT_ENVIRONMENT_CONFIG_OPTIONS,
      CT_MOUNT_ENVIRONMENT_CONFIG_ADD_PATH);
  if (environment_status != CT_MOUNT_ENVIRONMENT_OK) return environment_status;
  if (extra != NULL && extra[0] != '\0') {
    if (snprintf(extra_contents, sizeof(extra_contents), "%s", extra) >=
            (int)sizeof(extra_contents) ||
        ct_mount_tokenize(extra_contents, extra_tokens, &extra_count) != CT_MOUNT_OK) {
      return CT_MOUNT_ENVIRONMENT_EXTRA_OPTIONS;
    }
  }
  environment_status = ct_mount_validate_environment_options(
      extra_tokens, extra_count, CT_MOUNT_ENVIRONMENT_EXTRA_OPTIONS,
      CT_MOUNT_ENVIRONMENT_EXTRA_ADD_PATH);
  if (environment_status != CT_MOUNT_ENVIRONMENT_OK) return environment_status;
  if (ct_mount_format_args(file_tokens, file_count, extra_tokens, extra_count,
                            formatted, sizeof(formatted)) != CT_MOUNT_OK ||
      ct_mount_tokenize(formatted, formatted_tokens, &formatted_count) != CT_MOUNT_OK) {
    return CT_MOUNT_ENVIRONMENT_COMBINED_OPTIONS;
  }
  if (ct_mount_collect((int)formatted_count, (char *const *)formatted_tokens,
                        paths, path_count) != 0) return CT_MOUNT_ENVIRONMENT_DISCOVERY;
  for (index = 0U; index < *path_count; ++index) {
    if (!ct_mount_is_persistent_path(paths[index])) {
      *path_count = 0U;
      return CT_MOUNT_ENVIRONMENT_DETECTED_PATH;
    }
  }
  return CT_MOUNT_ENVIRONMENT_OK;
}
