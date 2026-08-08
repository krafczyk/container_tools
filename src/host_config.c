/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "host_config.h"

#include "tomlc17.h"

#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CT_HOST_CONFIG_MAX_BYTES 1048576U
#define CT_HOST_MAX_PROFILES 64U

int ct_host_copy_bounded(char *destination, size_t size, const char *source)
{
  const size_t length = source == NULL ? size : strlen(source);
  if (source == NULL || length >= size) return 1;
  memcpy(destination, source, length + 1U);
  return 0;
}

int ct_host_path_validate(const char *value)
{
  const char *part;
  if (value == NULL || value[0] != '/') return 1;
  if (strcmp(value, "/") == 0) return 0;
  for (part = value + 1; ; ) {
    const char *end = strchr(part, '/');
    const size_t length = end == NULL ? strlen(part) : (size_t)(end - part);
    if (length == 0U || (length == 1U && part[0] == '.') || (length == 2U && part[0] == '.' && part[1] == '.')) return 1;
    if (end == NULL) return 0;
    part = end + 1;
  }
}

static int ct_host_name(const char *value)
{
  size_t index;
  if (value == NULL || value[0] == '\0' || strlen(value) >= 128U) return 1;
  for (index = 0U; value[index] != '\0'; ++index) if (!((value[index] >= 'A' && value[index] <= 'Z') || (value[index] >= 'a' && value[index] <= 'z') || (value[index] >= '0' && value[index] <= '9') || value[index] == '_')) return 1;
  return 0;
}

static int ct_host_environment_name(const char *value)
{
  return ct_host_name(value) != 0 ||
                 !((value[0] >= 'A' && value[0] <= 'Z') ||
                   (value[0] >= 'a' && value[0] <= 'z') || value[0] == '_')
             ? 1
             : 0;
}

static int ct_host_table_keys(toml_datum_t table, const char *const keys[], size_t key_count)
{
  int index;
  if (table.type != TOML_TABLE || table.u.tab.size < 0) return 1;
  for (index = 0; index < table.u.tab.size; ++index) {
    size_t key_index;
    bool found = false;
    for (key_index = 0U; key_index < key_count; ++key_index) if (strcmp(table.u.tab.key[index], keys[key_index]) == 0) { found = true; break; }
    if (!found) return 1;
  }
  return 0;
}

static int ct_host_string(toml_datum_t table, const char *key, char *destination, size_t size, int required)
{
  toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) return required != 0 ? 1 : 0;
  return value.type != TOML_STRING || ct_host_copy_bounded(destination, size, value.u.str.ptr) != 0 ? 1 : 0;
}

static int ct_host_enum(const char *value, const char *first, const char *second)
{
  return value[0] == '\0' || (strcmp(value, first) != 0 && strcmp(value, second) != 0) ? 1 : 0;
}

static int ct_host_parse_path_array(toml_datum_t table, const char *key, char output[][CT_HOST_PATH_MAX], size_t *count, size_t maximum)
{
  toml_datum_t value = toml_get(table, key);
  int index;
  if (value.type == TOML_UNKNOWN) return 0;
  if (value.type != TOML_ARRAY || value.u.arr.size < 0 || (size_t)value.u.arr.size > maximum) return 1;
  for (index = 0; index < value.u.arr.size; ++index) {
    if (value.u.arr.elem[index].type != TOML_STRING || ct_host_copy_bounded(output[index], CT_HOST_PATH_MAX, value.u.arr.elem[index].u.str.ptr) != 0 || ct_host_path_validate(output[index]) != 0) return 1;
    if (strchr(output[index], ':') != NULL) return 1;
  }
  *count = (size_t)value.u.arr.size;
  return 0;
}

static int ct_host_parse_profile(toml_datum_t table, const char *name, struct ct_host_profile *profile)
{
  static const char *const keys[] = {"root", "root_access", "semantics", "mount_plan", "cwd_unmapped", "path", "environment_remove", "environment", "projections"};
  toml_datum_t environment, projections;
  int index;
  memset(profile, 0, sizeof(*profile));
  if (ct_host_table_keys(table, keys, sizeof(keys) / sizeof(keys[0])) != 0 || ct_host_copy_bounded(profile->name, sizeof(profile->name), name) != 0 ||
      ct_host_string(table, "root", profile->root, sizeof(profile->root), 1) != 0 || ct_host_path_validate(profile->root) != 0 ||
      ct_host_string(table, "root_access", profile->root_access, sizeof(profile->root_access), 1) != 0 || ct_host_enum(profile->root_access, "inherit", "read-only") != 0 ||
      ct_host_string(table, "semantics", profile->semantics, sizeof(profile->semantics), 1) != 0 || ct_host_enum(profile->semantics, "full-root", "rewrite") != 0 ||
      ct_host_string(table, "cwd_unmapped", profile->cwd_unmapped, sizeof(profile->cwd_unmapped), 1) != 0 || ct_host_enum(profile->cwd_unmapped, "error", "root") != 0 ||
      ct_host_parse_path_array(table, "path", profile->path, &profile->path_count, CT_HOST_MAX_PATHS) != 0 || profile->path_count == 0U) return 1;
  if (ct_host_string(table, "mount_plan", profile->mount_plan, sizeof(profile->mount_plan), 0) != 0) return 1;
  profile->mount_plan_configured = profile->mount_plan[0] != '\0';
  if (profile->mount_plan_configured != 0 && ct_host_path_validate(profile->mount_plan) != 0) return 1;
  {
    toml_datum_t remove = toml_get(table, "environment_remove");
    if (remove.type != TOML_UNKNOWN) {
      if (remove.type != TOML_ARRAY || remove.u.arr.size < 0 || (size_t)remove.u.arr.size > CT_HOST_MAX_ENVIRONMENT) return 1;
      for (index = 0; index < remove.u.arr.size; ++index) if (remove.u.arr.elem[index].type != TOML_STRING || ct_host_copy_bounded(profile->environment_remove[index], sizeof(profile->environment_remove[index]), remove.u.arr.elem[index].u.str.ptr) != 0 || ct_host_environment_name(profile->environment_remove[index]) != 0) return 1;
      profile->environment_remove_count = (size_t)remove.u.arr.size;
    }
  }
  environment = toml_get(table, "environment");
  if (environment.type != TOML_UNKNOWN) {
    if (environment.type != TOML_TABLE || environment.u.tab.size < 0 || (size_t)environment.u.tab.size > CT_HOST_MAX_ENVIRONMENT) return 1;
    for (index = 0; index < environment.u.tab.size; ++index) {
      if (ct_host_environment_name(environment.u.tab.key[index]) != 0 || environment.u.tab.value[index].type != TOML_STRING ||
          ct_host_copy_bounded(profile->environment[index].name, sizeof(profile->environment[index].name), environment.u.tab.key[index]) != 0 ||
          ct_host_copy_bounded(profile->environment[index].value, sizeof(profile->environment[index].value), environment.u.tab.value[index].u.str.ptr) != 0) return 1;
      if (strcmp(profile->environment[index].name, "PATH") == 0) return 1;
    }
    profile->environment_count = (size_t)environment.u.tab.size;
  }
  if (profile->environment_remove_count + profile->environment_count >
      CT_HOST_MAX_ENVIRONMENT) return 1;
  for (index = 0; index < (int)profile->environment_remove_count; ++index) {
    size_t other;
    if (strcmp(profile->environment_remove[index], "PATH") == 0) return 1;
    for (other = 0U; other < profile->environment_count; ++other) if (strcmp(profile->environment_remove[index], profile->environment[other].name) == 0) return 1;
  }
  projections = toml_get(table, "projections");
  if (projections.type == TOML_UNKNOWN) return 0;
  if (projections.type != TOML_ARRAY || projections.u.arr.size < 0 || (size_t)projections.u.arr.size > CT_HOST_MAX_PROJECTIONS) return 1;
  for (index = 0; index < projections.u.arr.size; ++index) {
    static const char *const projection_keys[] = {"visible", "target", "access", "required"};
    toml_datum_t entry = projections.u.arr.elem[index];
    toml_datum_t required;
    if (ct_host_table_keys(entry, projection_keys, sizeof(projection_keys) / sizeof(projection_keys[0])) != 0 ||
        ct_host_string(entry, "visible", profile->projections[index].visible, sizeof(profile->projections[index].visible), 1) != 0 || ct_host_path_validate(profile->projections[index].visible) != 0 ||
        ct_host_string(entry, "target", profile->projections[index].target, sizeof(profile->projections[index].target), 1) != 0 || ct_host_path_validate(profile->projections[index].target) != 0 || strcmp(profile->projections[index].target, "/") == 0 ||
        ct_host_string(entry, "access", profile->projections[index].access, sizeof(profile->projections[index].access), 1) != 0 || ct_host_enum(profile->projections[index].access, "inherit", "read-only") != 0) return 1;
    required = toml_get(entry, "required");
    if (required.type != TOML_BOOLEAN) return 1;
    profile->projections[index].required = required.u.boolean ? 1 : 0;
    for (int previous = 0; previous < index; ++previous) {
      if (strcmp(profile->projections[previous].target,
                 profile->projections[index].target) == 0) return 1;
    }
  }
  profile->projection_count = (size_t)projections.u.arr.size;
  return 0;
}

enum ct_host_config_status ct_host_config_parse(const char *contents,
                                                size_t length,
                                                const char *selected_profile,
                                                struct ct_host_profile *profile)
{
  static const char *const keys[] = {"version", "default_profile", "profiles"};
  toml_result_t parsed;
  toml_datum_t version, profiles;
  char default_profile[128] = "";
  const char *wanted = selected_profile;
  struct ct_host_profile *candidate;
  int index, selected_index = -1;
  bool default_found = false;
  if (contents == NULL || profile == NULL || length > CT_HOST_CONFIG_MAX_BYTES ||
      contents[length] != '\0' || memchr(contents, '\0', length) != NULL ||
      (selected_profile != NULL && ct_host_name(selected_profile) != 0)) return CT_HOST_CONFIG_INVALID;
  candidate = malloc(sizeof(*candidate));
  if (candidate == NULL) return CT_HOST_CONFIG_IO;
  parsed = toml_parse(contents, (int)length);
  if (!parsed.ok || ct_host_table_keys(parsed.toptab, keys, sizeof(keys) / sizeof(keys[0])) != 0) { toml_free(parsed); free(candidate); return CT_HOST_CONFIG_INVALID; }
  version = toml_get(parsed.toptab, "version"); profiles = toml_get(parsed.toptab, "profiles");
  if (version.type != TOML_INT64 || version.u.int64 != 1 || profiles.type != TOML_TABLE || profiles.u.tab.size < 1 || profiles.u.tab.size > (int)CT_HOST_MAX_PROFILES || ct_host_string(parsed.toptab, "default_profile", default_profile, sizeof(default_profile), 1) != 0 || ct_host_name(default_profile) != 0) { toml_free(parsed); free(candidate); return CT_HOST_CONFIG_INVALID; }
  if (wanted == NULL) wanted = default_profile;
  for (index = 0; index < profiles.u.tab.size; ++index) {
    if (ct_host_name(profiles.u.tab.key[index]) != 0 ||
        ct_host_parse_profile(profiles.u.tab.value[index],
                              profiles.u.tab.key[index], candidate) != 0) {
      toml_free(parsed);
      free(candidate);
      return CT_HOST_CONFIG_INVALID;
    }
    if (strcmp(profiles.u.tab.key[index], default_profile) == 0) default_found = true;
    if (strcmp(profiles.u.tab.key[index], wanted) == 0) selected_index = index;
  }
  if (!default_found || selected_index < 0 ||
      ct_host_parse_profile(profiles.u.tab.value[selected_index], wanted,
                            candidate) != 0) {
    toml_free(parsed);
    free(candidate);
    return CT_HOST_CONFIG_INVALID;
  }
  *profile = *candidate;
  toml_free(parsed);
  free(candidate);
  return CT_HOST_CONFIG_OK;
}

static enum ct_host_config_status ct_host_read(const char *path, const char *selected_profile, struct ct_host_profile *profile, int absent_ok)
{
  struct stat before, after, final;
  char *contents;
  int descriptor;
  size_t used = 0U;
  enum ct_host_config_status result;
  if (path == NULL || ct_host_path_validate(path) != 0) return CT_HOST_CONFIG_INVALID;
  descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0) return absent_ok != 0 && errno == ENOENT ? CT_HOST_CONFIG_ABSENT : CT_HOST_CONFIG_IO;
  if (fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size < 0 || (size_t)before.st_size > CT_HOST_CONFIG_MAX_BYTES) {
    (void)close(descriptor);
    return CT_HOST_CONFIG_INVALID;
  }
  contents = calloc((size_t)before.st_size + 1U, 1U);
  if (contents == NULL) { (void)close(descriptor); return CT_HOST_CONFIG_IO; }
  while (used < (size_t)before.st_size) {
    const ssize_t received = read(descriptor, contents + used,
                                  (size_t)before.st_size - used);
    if (received <= 0) {
      (void)close(descriptor);
      free(contents);
      return CT_HOST_CONFIG_IO;
    }
    used += (size_t)received;
  }
  if (fstat(descriptor, &after) != 0 || close(descriptor) != 0 ||
      stat(path, &final) != 0 || !S_ISREG(final.st_mode) ||
      before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
      before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_dev != final.st_dev || before.st_ino != final.st_ino ||
      before.st_size != final.st_size ||
      before.st_mtim.tv_sec != final.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != final.st_mtim.tv_nsec) {
    free(contents);
    return CT_HOST_CONFIG_IO;
  }
  result = ct_host_config_parse(contents, used, selected_profile, profile);
  free(contents);
  return result;
}

enum ct_host_config_status ct_host_config_load(const char *explicit_path, const char *selected_profile, struct ct_host_profile *profile)
{
  const char *environment_path;
  const char *xdg;
  const char *home;
  char path[CT_HOST_PATH_MAX];
  enum ct_host_config_status result;
  if (profile == NULL) return CT_HOST_CONFIG_INVALID;
  if (explicit_path != NULL) return ct_host_read(explicit_path, selected_profile, profile, 0);
  environment_path = getenv("CONTAINER_TOOLS_HOST_CONFIG");
  if (environment_path != NULL && environment_path[0] != '\0') return ct_host_read(environment_path, selected_profile, profile, 0);
  xdg = getenv("XDG_CONFIG_HOME"); home = getenv("HOME");
  if (xdg != NULL && xdg[0] == '/') {
    if (strcmp(xdg, "/") == 0
            ? ct_host_copy_bounded(path, sizeof(path),
                                   "/container-tools/host.toml") != 0
            : snprintf(path, sizeof(path), "%s/container-tools/host.toml",
                       xdg) >= (int)sizeof(path)) return CT_HOST_CONFIG_INVALID;
    result = ct_host_read(path, selected_profile, profile, 1);
    if (result != CT_HOST_CONFIG_ABSENT) return result;
  }
  if (home == NULL || home[0] != '/') return CT_HOST_CONFIG_ABSENT;
  if (strcmp(home, "/") == 0
          ? ct_host_copy_bounded(path, sizeof(path),
                                 "/.config/container-tools/host.toml") != 0
          : snprintf(path, sizeof(path), "%s/.config/container-tools/host.toml",
                     home) >= (int)sizeof(path)) return CT_HOST_CONFIG_INVALID;
  return ct_host_read(path, selected_profile, profile, 1);
}
