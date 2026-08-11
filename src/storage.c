/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "storage.h"
#include "cli.h"
#include "storage_timeout.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool ct_storage_path_is_safe(const char *path)
{
  size_t length;

  if (path == NULL || path[0] != '/' || strchr(path, '\n') != NULL) return false;
  length = strlen(path);
  return strstr(path, "/../") == NULL && strstr(path, "/./") == NULL &&
         !(length >= 2U && strcmp(path + length - 2U, "/.") == 0) &&
         !(length >= 3U && strcmp(path + length - 3U, "/..") == 0);
}

static int ct_storage_check_ancestors(const char *path)
{
  char ancestor[CT_CONFIG_PATH_MAX];
  char *slash;

  if (strlen(path) >= sizeof(ancestor)) return 1;
  (void)snprintf(ancestor, sizeof(ancestor), "%s", path);
  slash = strrchr(ancestor, '/');
  if (slash == NULL) return 1;
  if (slash == ancestor) ancestor[1] = '\0'; else *slash = '\0';
  for (;;) {
    struct stat status;
    if (ct_storage_timeout_lstat(ancestor, &status) != 0) {
      if (errno != ENOENT) return 1;
    } else if (!S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || ((status.st_mode & 0022) != 0 &&
                                    (status.st_mode & 01000) == 0)) return 1;
    if (strcmp(ancestor, "/") == 0) return 0;
    slash = strrchr(ancestor, '/');
    if (slash == ancestor) ancestor[1] = '\0'; else *slash = '\0';
  }
}

static int ct_storage_ensure_private_directory_inner(const char *path)
{
  struct stat status;

  if (!ct_storage_path_is_safe(path) || ct_storage_check_ancestors(path) != 0) return 1;
  if (ct_storage_timeout_lstat(path, &status) != 0) {
    char parent[CT_CONFIG_PATH_MAX];
    char *slash;
    if (errno != ENOENT || strlen(path) >= sizeof(parent)) return 1;
    (void)snprintf(parent, sizeof(parent), "%s", path);
    slash = strrchr(parent, '/');
    if (slash == NULL) return 1;
    if (slash == parent) parent[1] = '\0'; else *slash = '\0';
    if ((ct_storage_timeout_lstat(parent, &status) != 0 && (errno != ENOENT ||
                                            ct_storage_ensure_private_directory_inner(parent) != 0)) ||
        (ct_storage_timeout_mkdir(path, 0700) != 0 && errno != EEXIST) ||
        ct_storage_timeout_lstat(path, &status) != 0 ||
        ct_storage_check_ancestors(path) != 0) return 1;
  }
  if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & 0022) != 0) return 1;
  return (status.st_mode & 0777) == 0700 || ct_storage_timeout_chmod(path, 0700) == 0 ? 0 : 1;
}

int ct_storage_ensure_private_directory(const char *path)
{
  return ct_storage_ensure_private_directory_inner(path);
}

static int ct_storage_select_directory(const char *variable, const char *fallback)
{
  const char *existing = getenv(variable);
  if (existing != NULL && existing[0] != '\0') return 0;
  if (fallback == NULL || fallback[0] == '\0') return 0;
  if (ct_storage_ensure_private_directory_inner(fallback) != 0) return 1;
  return setenv(variable, fallback, 1) == 0 ? 0 : 1;
}

static const char *ct_storage_configured_default(const char *variable, const char *configured)
{
  const char *override = getenv(variable);
  return override != NULL && override[0] != '\0' ? override : configured;
}

int ct_storage_select_runtime(const char *backend, const struct ct_runtime_config *config)
{
  int result;
  if (backend == NULL || config == NULL) {
    ct_cli_diagnostic("runtime storage", "configuration",
                      "select a supported runtime backend and valid runtime configuration, then retry");
    return 1;
  }
  if (!ct_storage_timeout_is_valid()) {
    ct_cli_diagnostic("runtime storage", "timeout",
                      "set CT_RUNTIME_STORAGE_TIMEOUT to a positive decimal no greater than 3600, then retry");
    return 1;
  }
  if (strcmp(backend, "singularity") == 0) {
    result = ct_storage_select_directory("SINGULARITY_CACHEDIR",
                                         ct_storage_configured_default("CT_SINGULARITY_CACHE_DIR", config->singularity_cache_dir)) ||
             ct_storage_select_directory("SINGULARITY_TMPDIR",
                                         ct_storage_configured_default("CT_SINGULARITY_TMP_DIR", config->singularity_tmp_dir));
  } else if (strcmp(backend, "apptainer") == 0) {
    result = ct_storage_select_directory("APPTAINER_CACHEDIR",
                                         ct_storage_configured_default("CT_SINGULARITY_CACHE_DIR", config->singularity_cache_dir)) ||
             ct_storage_select_directory("APPTAINER_TMPDIR",
                                         ct_storage_configured_default("CT_SINGULARITY_TMP_DIR", config->singularity_tmp_dir));
  } else {
    ct_cli_diagnostic("runtime storage", "backend",
                      "select singularity or apptainer storage, then retry");
    return 1;
  }
  if (result != 0) {
    ct_cli_diagnostic("runtime storage", "ownership-or-mode",
                      "make configured storage directories current-user owned with mode 0700, then retry");
  }
  return result;
}
