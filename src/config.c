/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "config.h"
#include "cli.h"
#include "storage_timeout.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CT_CONFIG_FILE_MAX 65536U

static void ct_config_diagnostic(const char *category, const char *action)
{
  ct_cli_diagnostic("runtime configuration", category, action);
}

static bool ct_config_path_is_valid(const char *value)
{
  return value != NULL && value[0] == '/' && strchr(value, '\n') == NULL &&
         strlen(value) < CT_CONFIG_PATH_MAX;
}

static char *ct_config_slot(struct ct_runtime_config *config, const char *key)
{
  if (strcmp(key, "CT_SINGULARITY_CACHE_DIR") == 0) return config->singularity_cache_dir;
  if (strcmp(key, "CT_SINGULARITY_TMP_DIR") == 0) return config->singularity_tmp_dir;
  return NULL;
}

enum ct_config_status ct_runtime_config_parse(const char *contents,
                                              struct ct_runtime_config *config)
{
  char copy[CT_CONFIG_FILE_MAX + 1U];
  char *line;
  char *cursor;
  size_t length;

  if (contents == NULL || config == NULL || (length = strlen(contents)) > CT_CONFIG_FILE_MAX) {
    return CT_CONFIG_INVALID;
  }
  memset(config, 0, sizeof(*config));
  memcpy(copy, contents, length + 1U);
  cursor = copy;
  while (cursor != NULL) {
    char *equals;
    char *slot;

    line = cursor;
    cursor = strchr(cursor, '\n');
    if (cursor != NULL) {
      *cursor = '\0';
      ++cursor;
    }

    if (line[0] == '\0' || line[0] == '#' || strspn(line, " \t\r") == strlen(line)) continue;
    if (line[strlen(line) - 1U] == '\r') line[strlen(line) - 1U] = '\0';
    equals = strchr(line, '=');
    if (equals == NULL) return CT_CONFIG_INVALID;
    *equals = '\0';
    slot = ct_config_slot(config, line);
    if (slot == NULL || slot[0] != '\0' || !ct_config_path_is_valid(equals + 1)) {
      return CT_CONFIG_INVALID;
    }
    (void)snprintf(slot, CT_CONFIG_PATH_MAX, "%s", equals + 1);
  }
  return CT_CONFIG_OK;
}

enum ct_config_status ct_runtime_config_load(const char *path,
                                             struct ct_runtime_config *config)
{
  char contents[CT_CONFIG_FILE_MAX + 1U];
  struct stat status;
  int descriptor;
  size_t used = 0U;

  if (path == NULL || config == NULL || path[0] != '/') {
    ct_config_diagnostic("configuration",
                         "set CT_RUNTIME_CFG to an owned regular file containing only supported absolute-path variables, then retry");
    return CT_CONFIG_INVALID;
  }
  descriptor = ct_storage_timeout_open(path, O_RDONLY | O_CLOEXEC, 0);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      memset(config, 0, sizeof(*config));
      return CT_CONFIG_OK;
    }
    ct_config_diagnostic("storage", "fix the configuration file access and retry");
    return CT_CONFIG_IO;
  }
  if (ct_storage_timeout_fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 0022) != 0) {
    (void)ct_storage_timeout_close(descriptor);
    ct_config_diagnostic("ownership-or-mode",
                         "make the configuration file owned by the current user and not group- or other-writable, then retry");
    return CT_CONFIG_INVALID;
  }
  while (used + 1U < sizeof(contents)) {
    const ssize_t read_count = ct_storage_timeout_read(descriptor, contents + used,
                                                       sizeof(contents) - 1U - used);
    if (read_count < 0) {
      if (errno == EINTR) continue;
      (void)ct_storage_timeout_close(descriptor);
      ct_config_diagnostic("storage", "fix the configuration file access and retry");
      return CT_CONFIG_IO;
    }
    if (read_count == 0) break;
    used += (size_t)read_count;
  }
  if (ct_storage_timeout_close(descriptor) != 0) {
    ct_config_diagnostic("storage", "fix the configuration file access and retry");
    return CT_CONFIG_IO;
  }
  if (used + 1U >= sizeof(contents) || memchr(contents, '\0', used) != NULL) {
    ct_config_diagnostic("configuration",
                         "keep the configuration text-only and within the supported size, then retry");
    return CT_CONFIG_INVALID;
  }
  contents[used] = '\0';
  {
    const enum ct_config_status result = ct_runtime_config_parse(contents, config);
    if (result != CT_CONFIG_OK) {
      ct_config_diagnostic("configuration",
                           "set CT_RUNTIME_CFG to an owned regular file containing only supported absolute-path variables, then retry");
    }
    return result;
  }
}

enum ct_config_status ct_runtime_config_load_environment(struct ct_runtime_config *config)
{
  char path[CT_CONFIG_PATH_MAX];
  const char *configured = getenv("CT_RUNTIME_CFG");
  const char *home = getenv("HOME");

  if (configured != NULL && configured[0] != '\0') return ct_runtime_config_load(configured, config);
  if (home == NULL || home[0] != '/') {
    ct_config_diagnostic("configuration",
                         "set HOME or CT_RUNTIME_CFG to an absolute configuration location, then retry");
    return CT_CONFIG_INVALID;
  }
  if (snprintf(path, sizeof(path), "%s/.config/ct_runtime.conf", home) >= (int)sizeof(path)) {
    ct_config_diagnostic("configuration",
                         "set HOME or CT_RUNTIME_CFG to an absolute configuration location, then retry");
    return CT_CONFIG_INVALID;
  }
  return ct_runtime_config_load(path, config);
}
