/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "instance.h"

#include "backend_outer.h"
#include "cli.h"
#include "config.h"
#include "host_projection.h"
#include "mount.h"
#include "mount_plan.h"
#include "process.h"
#include "profile.h"
#include "state.h"
#include "storage.h"
#include "storage_timeout.h"

#include "yyjson.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern char *realpath(const char *restrict path, char *restrict resolved_path);

#define CT_INSTANCE_EXPLICIT_MAX 128U
#define CT_INSTANCE_ENVIRONMENT_MAX 128U
#define CT_INSTANCE_GROUP_MAX 1024U
#define CT_INSTANCE_MOUNT_MAX (CT_MOUNT_MAX_PATHS * 2U + CT_HOST_PROJECTION_MAX_ENTRIES * 2U + CT_INSTANCE_EXPLICIT_MAX * 2U + 16U)
#define CT_INSTANCE_FIXED_FIELD_MAX 16U
#define CT_INSTANCE_FIELD_MAX (CT_INSTANCE_MOUNT_MAX * 3U + CT_INSTANCE_GROUP_MAX + CT_INSTANCE_FIXED_FIELD_MAX)
#define CT_INSTANCE_ARGUMENT_MAX (CT_INSTANCE_MOUNT_MAX * 2U + CT_INSTANCE_ENVIRONMENT_MAX * 2U + 32U)
#define CT_INSTANCE_PATH_MAX 4096U
#define CT_INSTANCE_DESCRIPTOR_MAX (CT_INSTANCE_PATH_MAX * 2U + 64U)

_Static_assert(CT_INSTANCE_FIELD_MAX <= CT_PROFILE_MAX_FIELDS,
               "profile field capacity must cover persistent instances");

struct ct_instance_bind {
  char source[CT_INSTANCE_PATH_MAX];
  char target[CT_INSTANCE_PATH_MAX];
};

struct ct_instance_request {
  const char *backend;
  const char *runtime_argument;
  char root[CT_INSTANCE_PATH_MAX];
  const char *image;
  const char *bootstrap;
  const char *host_root;
  const char *container_shell;
  int refresh;
  int delimited;
  char *payload[CT_INSTANCE_ARGUMENT_MAX];
  size_t payload_count;
  struct ct_instance_bind binds[CT_INSTANCE_EXPLICIT_MAX];
  size_t bind_count;
  const char *environment[CT_INSTANCE_ENVIRONMENT_MAX];
  size_t environment_count;
};

struct ct_instance_mount {
  char *source;
  char *target;
  char *source_real;
  char *descriptor;
  char *profile_identity;
  const char *role;
  const char *access;
  const char *recursion;
  int generated;
  int semantic;
};

struct ct_instance_prepared {
  struct ct_instance_mount *mounts;
  size_t mount_count;
  enum ct_mount_environment_status mount_environment_status;
  char digest[CT_PROFILE_DIGEST_HEX_LENGTH];
  char name[40];
  char image_real[CT_INSTANCE_PATH_MAX];
  char image_identity[256];
  char manifest[CT_INSTANCE_PATH_MAX];
  char cwd[CT_INSTANCE_PATH_MAX];
  char cwd_real[CT_INSTANCE_PATH_MAX];
  char user[256];
};

enum ct_instance_prepare_status {
  CT_INSTANCE_PREPARE_OK = 0,
  CT_INSTANCE_PREPARE_IMAGE = 1,
  CT_INSTANCE_PREPARE_BOOTSTRAP = 2,
  CT_INSTANCE_PREPARE_HOME = 3,
  CT_INSTANCE_PREPARE_CWD = 4,
  CT_INSTANCE_PREPARE_PROJECTION = 5,
  CT_INSTANCE_PREPARE_MOUNT_CONFIG = 6,
  CT_INSTANCE_PREPARE_BIND = 7,
  CT_INSTANCE_PREPARE_MOUNT_PLAN = 8,
  CT_INSTANCE_PREPARE_ASSETS = 9,
  CT_INSTANCE_PREPARE_INTERNAL = 10,
};

static void ct_instance_diagnostic(const char *category, const char *action)
{
  ct_cli_diagnostic("persistent instance", category, action);
}

static void ct_instance_recovery_diagnostic(const char *category,
                                            const char *action)
{
  ct_cli_diagnostic("persistent instance recovery", category, action);
}

static int ct_instance_backend(const char *value)
{
  return value != NULL && (strcmp(value, "--apptainer") == 0 ||
                           strcmp(value, "--singularity") == 0);
}

static int ct_instance_path_scalar(const char *value)
{
  return value != NULL && value[0] == '/' && strchr(value, ':') == NULL &&
         strchr(value, ',') == NULL && strchr(value, '\n') == NULL;
}

static int ct_instance_normalize_path(const char *value,
                                      char output[CT_INSTANCE_PATH_MAX])
{
  char copy[CT_INSTANCE_PATH_MAX];
  char *components[CT_INSTANCE_PATH_MAX / 2U];
  char *cursor, *state = NULL;
  size_t count = 0U, used = 1U, index;

  if (!ct_instance_path_scalar(value) ||
      snprintf(copy, sizeof(copy), "%s", value) >= (int)sizeof(copy)) return 1;
  cursor = strtok_r(copy, "/", &state);
  while (cursor != NULL) {
    if (strcmp(cursor, ".") == 0) {
      cursor = strtok_r(NULL, "/", &state);
      continue;
    }
    if (strcmp(cursor, "..") == 0) {
      if (count > 0U) --count;
    } else {
      if (count == sizeof(components) / sizeof(components[0])) return 1;
      components[count++] = cursor;
    }
    cursor = strtok_r(NULL, "/", &state);
  }
  output[0] = '/';
  output[1] = '\0';
  for (index = 0U; index < count; ++index) {
    const size_t length = strlen(components[index]);
    if (used + (used > 1U ? 1U : 0U) + length >= CT_INSTANCE_PATH_MAX) return 1;
    if (used > 1U) output[used++] = '/';
    memcpy(output + used, components[index], length);
    used += length;
    output[used] = '\0';
  }
  return 0;
}

static int ct_instance_canonicalize_state_root(
    const char *value, char output[CT_INSTANCE_PATH_MAX])
{
  char normalized[CT_INSTANCE_PATH_MAX], prefix[CT_INSTANCE_PATH_MAX];
  char resolved[CT_INSTANCE_PATH_MAX], combined[CT_INSTANCE_PATH_MAX];
  char *slash;
  size_t prefix_length;

  if (ct_instance_normalize_path(value, normalized) != 0 ||
      snprintf(prefix, sizeof(prefix), "%s", normalized) >=
          (int)sizeof(prefix)) return 1;
  while (realpath(prefix, resolved) == NULL) {
    if (errno != ENOENT && errno != ENOTDIR) return 1;
    slash = strrchr(prefix, '/');
    if (slash == NULL || slash == prefix) {
      prefix[1] = '\0';
    } else {
      *slash = '\0';
    }
    if (strcmp(prefix, "/") == 0 && realpath(prefix, resolved) == NULL) return 1;
  }
  prefix_length = strlen(prefix);
  if (snprintf(combined, sizeof(combined), "%s%s", resolved,
               normalized + prefix_length) >= (int)sizeof(combined)) return 1;
  return ct_instance_normalize_path(combined, output);
}

static int ct_instance_environment(const char *value)
{
  const char *equals = value == NULL ? NULL : strchr(value, '=');
  const char *cursor;
  size_t name_length;

  if (equals == NULL || equals == value ||
      !(value[0] == '_' || (value[0] >= 'A' && value[0] <= 'Z') ||
        (value[0] >= 'a' && value[0] <= 'z'))) return 0;
  for (cursor = value + 1; cursor < equals; ++cursor) {
    if (!(*cursor == '_' || (*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= 'a' && *cursor <= 'z') ||
          (*cursor >= '0' && *cursor <= '9'))) return 0;
  }
  for (cursor = equals + 1; *cursor != '\0'; ++cursor) {
    if (!((*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= 'a' && *cursor <= 'z') ||
          (*cursor >= '0' && *cursor <= '9') ||
          strchr("_./:@%+ =-", *cursor) != NULL)) return 0;
  }
  name_length = (size_t)(equals - value);
  return !(name_length == strlen("SINGULARITYENV_CONTAINER_TOOLS_PROFILE") &&
           strncmp(value, "SINGULARITYENV_CONTAINER_TOOLS_PROFILE",
                   name_length) == 0) &&
         !(name_length == strlen("CT_INSTANCE_PROFILE") &&
           strncmp(value, "CT_INSTANCE_PROFILE", name_length) == 0);
}

static int ct_instance_parse(int argument_count, char *const arguments[],
                             struct ct_instance_request *request)
{
  int index;
  int host_root_seen = 0, shell_seen = 0;

  if (argument_count < 6 || arguments == NULL || request == NULL ||
      !ct_instance_backend(arguments[0]) ||
      strcmp(arguments[1], "--ct-instance-root") != 0 ||
      ct_instance_canonicalize_state_root(arguments[2], request->root) != 0) return 1;
  memset(request->binds, 0, sizeof(request->binds));
  request->backend = arguments[0] + 2;
  request->runtime_argument = getenv("CT_SINGULARITY_ARGS");
  if (request->runtime_argument != NULL && request->runtime_argument[0] == '\0') {
    request->runtime_argument = NULL;
  }
  request->image = NULL;
  request->bootstrap = NULL;
  request->host_root = "auto";
  request->container_shell = "/bin/sh";
  request->refresh = 0;
  request->delimited = 0;
  request->payload_count = 0U;
  request->bind_count = 0U;
  request->environment_count = 0U;
  for (index = 3; index < argument_count; ++index) {
    if (strcmp(arguments[index], "--") == 0) {
      request->delimited = 1;
      ++index;
      for (; index < argument_count; ++index) {
        if (request->payload_count + 1U >= CT_INSTANCE_ARGUMENT_MAX) return 1;
        request->payload[request->payload_count++] = arguments[index];
      }
      break;
    }
    if (strcmp(arguments[index], "--ct-bind") == 0) {
      char *separator;
      struct ct_instance_bind *bind;
      if (++index >= argument_count || request->bind_count == CT_INSTANCE_EXPLICIT_MAX ||
          (separator = strchr(arguments[index], ':')) == NULL ||
          separator == arguments[index] || strchr(separator + 1, ':') != NULL) return 1;
      bind = &request->binds[request->bind_count];
      if ((size_t)(separator - arguments[index]) >= sizeof(bind->source) ||
          snprintf(bind->source, sizeof(bind->source), "%.*s",
                   (int)(separator - arguments[index]), arguments[index]) >=
              (int)sizeof(bind->source) ||
          !ct_instance_path_scalar(bind->source) ||
          ct_instance_normalize_path(separator + 1, bind->target) != 0 ||
          strcmp(bind->target, "/.container-tools-instance-identity") == 0 ||
          strcmp(bind->target, "/.container-tools-mount-plan") == 0 ||
          strncmp(bind->target, "/.container-tools-mount-plan/", 30U) == 0) return 1;
      ++request->bind_count;
      continue;
    }
    if (strcmp(arguments[index], "--ct-env") == 0) {
      if (++index >= argument_count ||
          request->environment_count == CT_INSTANCE_ENVIRONMENT_MAX ||
          !ct_instance_environment(arguments[index])) return 1;
      request->environment[request->environment_count++] = arguments[index];
      continue;
    }
    if (strcmp(arguments[index], "--ct-bootstrap") == 0) {
      if (++index >= argument_count || request->bootstrap != NULL ||
          !ct_instance_path_scalar(arguments[index])) return 1;
      request->bootstrap = arguments[index];
      continue;
    }
    if (strcmp(arguments[index], "--ct-container-shell") == 0) {
      if (++index >= argument_count || shell_seen ||
          !ct_instance_path_scalar(arguments[index])) return 1;
      request->container_shell = arguments[index];
      shell_seen = 1;
      continue;
    }
    if (strcmp(arguments[index], "--ct-host-root") == 0) {
      if (++index >= argument_count || host_root_seen ||
          (strcmp(arguments[index], "auto") != 0 &&
           strcmp(arguments[index], "required") != 0)) return 1;
      request->host_root = arguments[index];
      host_root_seen = 1;
      continue;
    }
    if (strcmp(arguments[index], "--ct-host-root-refresh") == 0) {
      if (request->refresh) return 1;
      request->refresh = 1;
      continue;
    }
    for (; index < argument_count; ++index) {
      if (request->payload_count + 1U >= CT_INSTANCE_ARGUMENT_MAX) return 1;
      request->payload[request->payload_count++] = arguments[index];
    }
    break;
  }
  if (request->payload_count < 2U ||
      !ct_instance_path_scalar(request->payload[0]) ||
      (request->bootstrap != NULL && !request->delimited)) return 1;
  request->image = request->payload[0];
  return 0;
}

static const char *ct_instance_file_type(mode_t mode)
{
  if (S_ISREG(mode)) return "regular file";
  if (S_ISDIR(mode)) return "directory";
  if (S_ISCHR(mode)) return "character special file";
  if (S_ISBLK(mode)) return "block special file";
  if (S_ISFIFO(mode)) return "fifo";
  if (S_ISLNK(mode)) return "symbolic link";
  if (S_ISSOCK(mode)) return "socket";
  return "unknown";
}

static int ct_instance_stat_identity(const char *path, int timestamps,
                                     char output[256])
{
  struct stat status;
  if (path == NULL || stat(path, &status) != 0) return 1;
  if (!timestamps) {
    return snprintf(output, 256U, "%llu:%llu:%s",
                    (unsigned long long)status.st_dev,
                    (unsigned long long)status.st_ino,
                    ct_instance_file_type(status.st_mode)) >= 256;
  }
  {
    struct tm modified, changed;
    char modified_zone[16], changed_zone[16];
    if (localtime_r(&status.st_mtim.tv_sec, &modified) == NULL ||
        localtime_r(&status.st_ctim.tv_sec, &changed) == NULL ||
        strftime(modified_zone, sizeof(modified_zone), "%z", &modified) == 0U ||
        strftime(changed_zone, sizeof(changed_zone), "%z", &changed) == 0U) return 1;
    return snprintf(output, 256U,
                    "%llu:%llu:%lld:%04d-%02d-%02d %02d:%02d:%02d.%09ld %s:%04d-%02d-%02d %02d:%02d:%02d.%09ld %s",
                    (unsigned long long)status.st_dev,
                    (unsigned long long)status.st_ino, (long long)status.st_size,
                    modified.tm_year + 1900, modified.tm_mon + 1, modified.tm_mday,
                    modified.tm_hour, modified.tm_min, modified.tm_sec,
                    status.st_mtim.tv_nsec, modified_zone, changed.tm_year + 1900,
                    changed.tm_mon + 1, changed.tm_mday, changed.tm_hour,
                    changed.tm_min, changed.tm_sec, status.st_ctim.tv_nsec,
                    changed_zone) >= 256;
  }
}

static int ct_instance_regular_file(const char *path)
{
  struct stat status;
  return path != NULL && stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static int ct_instance_mount_add(struct ct_instance_prepared *prepared,
                                 const char *source, const char *target,
                                 const char *role, const char *access,
                                 const char *recursion, int generated,
                                 int semantic, int profile_identity)
{
  struct ct_instance_mount *mount;
  char real[CT_INSTANCE_PATH_MAX];
  char stat_identity[256];
  const char *flag;
  size_t identity_length;

  if (prepared == NULL || prepared->mount_count == CT_INSTANCE_MOUNT_MAX ||
      source == NULL || target == NULL || role == NULL || access == NULL ||
      recursion == NULL || realpath(source, real) == NULL) return 1;
  mount = &prepared->mounts[prepared->mount_count];
  memset(mount, 0, sizeof(*mount));
  mount->source = strdup(source);
  mount->target = strdup(target);
  mount->source_real = strdup(real);
  mount->descriptor = malloc(CT_INSTANCE_DESCRIPTOR_MAX);
  if (mount->source == NULL || mount->target == NULL || mount->source_real == NULL ||
      mount->descriptor == NULL) goto failed;
  if (generated) {
    if (ct_backend_outer_projection_mount("apptainer", source, target,
                                          mount->descriptor,
                                          CT_INSTANCE_DESCRIPTOR_MAX,
                                          &flag) != 0 || strcmp(flag, "--mount") != 0) goto failed;
  } else if (snprintf(mount->descriptor, CT_INSTANCE_DESCRIPTOR_MAX, "%s:%s%s",
                      source, target, strcmp(access, "read-only") == 0 ? ":ro" : "") >=
                 (int)CT_INSTANCE_DESCRIPTOR_MAX) {
    goto failed;
  }
  if (profile_identity) {
    if (ct_instance_stat_identity(real, 0, stat_identity) != 0) goto failed;
    identity_length = strlen(mount->descriptor) + strlen(real) +
                      strlen(stat_identity) + 3U;
    mount->profile_identity = malloc(identity_length);
    if (mount->profile_identity == NULL ||
        snprintf(mount->profile_identity, identity_length, "%s:%s:%s",
                 mount->descriptor, real, stat_identity) >= (int)identity_length) goto failed;
  }
  mount->role = role;
  mount->access = access;
  mount->recursion = recursion;
  mount->generated = generated;
  mount->semantic = semantic;
  ++prepared->mount_count;
  return 0;
failed:
  free(mount->source);
  free(mount->target);
  free(mount->source_real);
  free(mount->descriptor);
  free(mount->profile_identity);
  memset(mount, 0, sizeof(*mount));
  return 1;
}

static void ct_instance_prepared_free(struct ct_instance_prepared *prepared)
{
  size_t index;
  if (prepared == NULL) return;
  for (index = 0U; index < prepared->mount_count; ++index) {
    free(prepared->mounts[index].source);
    free(prepared->mounts[index].target);
    free(prepared->mounts[index].source_real);
    free(prepared->mounts[index].descriptor);
    free(prepared->mounts[index].profile_identity);
  }
  free(prepared->mounts);
  prepared->mounts = NULL;
  prepared->mount_count = 0U;
}

static int ct_instance_path_inside(const char *path, const char *root)
{
  const size_t length = root == NULL ? 0U : strlen(root);
  return path != NULL && root != NULL && strncmp(path, root, length) == 0 &&
         (path[length] == '\0' || path[length] == '/');
}

static int ct_instance_target_taken(const struct ct_instance_prepared *prepared,
                                    const char *target)
{
  size_t index;
  for (index = 0U; index < prepared->mount_count; ++index) {
    if (strcmp(prepared->mounts[index].target, target) == 0) return 1;
  }
  return 0;
}

static int ct_instance_add_state_mask(struct ct_instance_prepared *prepared,
                                      const char *source, const char *target,
                                      const char *state_root)
{
  const char *relative;
  char alias[CT_INSTANCE_PATH_MAX], normalized[CT_INSTANCE_PATH_MAX];

  if (ct_instance_path_inside(source, state_root)) return 1;
  if (strcmp(source, "/") == 0) {
    relative = state_root;
  } else if (ct_instance_path_inside(state_root, source)) {
    relative = state_root + strlen(source);
  } else {
    return 0;
  }
  if (snprintf(alias, sizeof(alias), "%s%s", target, relative) >=
          (int)sizeof(alias) ||
      ct_instance_normalize_path(alias, normalized) != 0) return 1;
  if (ct_instance_target_taken(prepared, normalized)) {
    size_t index;
    for (index = 0U; index < prepared->mount_count; ++index) {
      if (strcmp(prepared->mounts[index].target, normalized) == 0 &&
          strcmp(prepared->mounts[index].role, "state-mask") == 0) return 0;
    }
    return 1;
  }
  return ct_instance_mount_add(prepared, state_root, normalized, "state-mask",
                               "read-only", "runtime-default", 0, 0, 0);
}

static enum ct_instance_prepare_status ct_instance_add_detected_mounts(
    struct ct_instance_prepared *prepared)
{
  char (*paths)[CT_MOUNT_PATH_MAX] = calloc(CT_MOUNT_MAX_PATHS, sizeof(*paths));
  size_t count = 0U, index;
  enum ct_instance_prepare_status result = CT_INSTANCE_PREPARE_INTERNAL;
  if (paths == NULL) goto done;
  prepared->mount_environment_status = ct_mount_collect_environment(paths, &count);
  if (prepared->mount_environment_status != CT_MOUNT_ENVIRONMENT_OK) {
    result = CT_INSTANCE_PREPARE_MOUNT_CONFIG;
    goto done;
  }
  for (index = 0U; index < count; ++index) {
    char target[CT_INSTANCE_PATH_MAX];
    if (ct_instance_normalize_path(paths[index], target) != 0) {
      prepared->mount_environment_status = CT_MOUNT_ENVIRONMENT_DETECTED_PATH;
      result = CT_INSTANCE_PREPARE_MOUNT_CONFIG;
      goto done;
    }
    if (strcmp(target, "/.container-tools-instance-identity") == 0 ||
        strcmp(target, "/.container-tools-bootstrap") == 0 ||
        strcmp(target, "/.container-tools-mount-plan") == 0 ||
        strncmp(target, "/.container-tools-mount-plan/", 30U) == 0) continue;
    if (ct_instance_mount_add(prepared, paths[index], target, "detected-automatic",
                              "inherit", "runtime-default", 0, 1, 1) != 0) {
      result = CT_INSTANCE_PREPARE_BIND;
      goto done;
    }
  }
  result = CT_INSTANCE_PREPARE_OK;
done:
  free(paths);
  return result;
}

static int ct_instance_field_append(const char **fields, size_t *field_count,
                                    const char *value)
{
  if (*field_count >= CT_INSTANCE_FIELD_MAX) return 1;
  fields[(*field_count)++] = value;
  return 0;
}

static int ct_instance_groups(const char **fields, size_t *field_count,
                              char values[][32], size_t value_count)
{
  gid_t group_storage[CT_INSTANCE_GROUP_MAX];
  gid_t *groups = group_storage;
  int count, index;
  size_t used = 0U;
  const char *configured = getenv("CT_HOST_PROJECTION_GROUPS");

  if (value_count > CT_INSTANCE_GROUP_MAX) return 1;
  if (configured != NULL && configured[0] != '\0') {
    char copy[4096], *token, *state = NULL;
    count = 0;
    if (snprintf(copy, sizeof(copy), "%s", configured) >= (int)sizeof(copy)) return 1;
    token = strtok_r(copy, " \t\r\n", &state);
    while (token != NULL) {
      char *end;
      unsigned long value;
      errno = 0;
      value = strtoul(token, &end, 10);
      if (errno != 0 || end == token || *end != '\0' || count == (int)value_count ||
          (unsigned long)(gid_t)value != value) return 1;
      groups[count++] = (gid_t)value;
      token = strtok_r(NULL, " \t\r\n", &state);
    }
  } else {
    count = getgroups(0, NULL);
    if (count < 0 || (size_t)count > value_count) return 1;
    if (count > 0 && getgroups(count, groups) != count) return 1;
  }
  for (index = 0; index < count; ++index) {
    int other;
    for (other = index + 1; other < count; ++other) {
      if (groups[other] < groups[index]) {
        const gid_t temporary = groups[index];
        groups[index] = groups[other];
        groups[other] = temporary;
      }
    }
  }
  for (index = 0; index < count; ++index) {
    if (groups[index] == getegid() || (index > 0 && groups[index] == groups[index - 1])) continue;
    if (used == value_count ||
        snprintf(values[used], sizeof(values[used]), "%lu",
                 (unsigned long)groups[index]) >= (int)sizeof(values[used])) {
      return 1;
    }
    if (ct_instance_field_append(fields, field_count, values[used++]) != 0) return 1;
  }
  return 0;
}

static int ct_instance_user(char output[256])
{
  struct passwd record;
  struct passwd *result = NULL;
  char buffer[4096];
  if (getpwuid_r(geteuid(), &record, buffer, sizeof(buffer), &result) != 0 ||
      result == NULL || result->pw_name == NULL ||
      snprintf(output, 256U, "%s", result->pw_name) >= 256) return 1;
  return 0;
}

static int ct_instance_state_root(char output[CT_INSTANCE_PATH_MAX])
{
  const char *override = getenv("CT_MOUNT_PLAN_STATE_ROOT");
  const char *base;
  char path[CT_INSTANCE_PATH_MAX];
  if (override != NULL && override[0] != '\0') {
    return ct_instance_normalize_path(override, output);
  }
  base = getenv("XDG_STATE_HOME");
  if (base != NULL && base[0] != '\0') {
    if (snprintf(path, sizeof(path), "%s/container-tools/mount-plans/v1", base) >=
        (int)sizeof(path)) return 1;
    return ct_instance_normalize_path(path, output);
  }
  base = getenv("HOME");
  if (base == NULL || base[0] != '/' ||
      snprintf(path, sizeof(path), "%s/.local/state/container-tools/mount-plans/v1",
               base) >= (int)sizeof(path)) return 1;
  return ct_instance_normalize_path(path, output);
}

static int ct_instance_assets_current(const struct ct_instance_request *request,
                                      const struct ct_instance_prepared *prepared,
                                      const char *bootstrap_real,
                                      const char *bootstrap_identity)
{
  char resolved[CT_INSTANCE_PATH_MAX], identity[256];
  if (realpath(request->image, resolved) == NULL ||
      strcmp(resolved, prepared->image_real) != 0 ||
      ct_instance_stat_identity(prepared->image_real, 1, identity) != 0 ||
      strcmp(identity, prepared->image_identity) != 0) return 0;
  if (request->bootstrap != NULL &&
      (realpath(request->bootstrap, resolved) == NULL ||
       strcmp(resolved, bootstrap_real) != 0 ||
       ct_instance_stat_identity(bootstrap_real, 1, identity) != 0 ||
       strcmp(identity, bootstrap_identity) != 0)) return 0;
  return 1;
}

static enum ct_instance_prepare_status ct_instance_prepare(
    const struct ct_instance_request *request, struct ct_instance_prepared *prepared)
{
  struct ct_host_projection projection;
  struct ct_mount_plan_entry *entries = NULL;
  const char **fields = NULL;
  const char **projection_fields = NULL;
  char (*group_values)[32] = NULL;
  char uid[32], gid[32], host[256], projection_digest[65];
  char bootstrap_real[CT_INSTANCE_PATH_MAX] = "", bootstrap_stat[256] = "";
  char bootstrap_identity[CT_INSTANCE_PATH_MAX + 257U] = "";
  char home_real[CT_INSTANCE_PATH_MAX], state_root[CT_INSTANCE_PATH_MAX];
  const char *home = getenv("HOME");
  size_t field_count = 0U, projection_count = 0U, entry_count = 0U, index;
  enum ct_instance_prepare_status result = CT_INSTANCE_PREPARE_INTERNAL;

  memset(prepared, 0, sizeof(*prepared));
  prepared->mounts = calloc(CT_INSTANCE_MOUNT_MAX, sizeof(*prepared->mounts));
  entries = calloc(CT_INSTANCE_MOUNT_MAX, sizeof(*entries));
  fields = calloc(CT_INSTANCE_FIELD_MAX, sizeof(*fields));
  projection_fields = calloc(CT_HOST_PROJECTION_MAX_ENTRIES * 6U + 4U,
                             sizeof(*projection_fields));
  group_values = calloc(CT_INSTANCE_GROUP_MAX, sizeof(*group_values));
  if (prepared->mounts == NULL || entries == NULL || fields == NULL ||
      projection_fields == NULL || group_values == NULL) goto done;
  if (home == NULL || ct_instance_normalize_path(home, home_real) != 0) {
    result = CT_INSTANCE_PREPARE_HOME;
    goto done;
  }
  if (getcwd(prepared->cwd, sizeof(prepared->cwd)) == NULL ||
      realpath(prepared->cwd, prepared->cwd_real) == NULL) {
    result = CT_INSTANCE_PREPARE_CWD;
    goto done;
  }
  if (realpath(request->image, prepared->image_real) == NULL ||
      !ct_instance_regular_file(prepared->image_real) ||
      access(prepared->image_real, R_OK) != 0 ||
      ct_instance_stat_identity(prepared->image_real, 1,
                                prepared->image_identity) != 0) {
    result = CT_INSTANCE_PREPARE_IMAGE;
    goto done;
  }
  if (gethostname(host, sizeof(host)) != 0 || ct_instance_user(prepared->user) != 0 ||
      snprintf(uid, sizeof(uid), "%lu", (unsigned long)geteuid()) >= (int)sizeof(uid) ||
      snprintf(gid, sizeof(gid), "%lu", (unsigned long)getegid()) >= (int)sizeof(gid)) goto done;
  host[sizeof(host) - 1U] = '\0';
  if (request->bootstrap != NULL) {
    if (realpath(request->bootstrap, bootstrap_real) == NULL ||
        !ct_instance_regular_file(bootstrap_real) ||
        access(bootstrap_real, R_OK | X_OK) != 0 ||
        ct_instance_stat_identity(bootstrap_real, 1, bootstrap_stat) != 0 ||
        snprintf(bootstrap_identity, sizeof(bootstrap_identity), "%s:%s",
                 bootstrap_real, bootstrap_stat) >= (int)sizeof(bootstrap_identity)) {
      result = CT_INSTANCE_PREPARE_BOOTSTRAP;
      goto done;
    }
  }
  if (ct_host_projection_prepare(request->backend, request->image,
                                 request->host_root, request->refresh,
                                 &projection) != 0 ||
      (strcmp(request->host_root, "required") == 0 &&
       (strcmp(projection.strategy, "none") == 0 ||
        strcmp(projection.completeness, "complete") != 0))) {
    result = CT_INSTANCE_PREPARE_PROJECTION;
    goto done;
  }
  for (index = 0U; index < projection.entry_count; ++index) {
    if (ct_instance_mount_add(prepared, projection.entries[index].source,
                              projection.entries[index].destination,
                              "generated-host-root", "inherit", "runtime-default",
                              1, 1, 0) != 0) {
      result = CT_INSTANCE_PREPARE_PROJECTION;
      goto done;
    }
  }
  result = ct_instance_add_detected_mounts(prepared);
  if (result != CT_INSTANCE_PREPARE_OK) {
    goto done;
  }
  for (index = 0U; index < request->bind_count; ++index) {
    if (ct_instance_mount_add(prepared, request->binds[index].source,
                              request->binds[index].target, "explicit", "inherit",
                              "runtime-default", 0, 1, 1) != 0) {
      result = CT_INSTANCE_PREPARE_BIND;
      goto done;
    }
  }
  if (request->bootstrap != NULL &&
      ct_instance_mount_add(prepared, bootstrap_real,
                            "/.container-tools-bootstrap", "bootstrap-internal",
                            "read-only", "runtime-default", 0, 1, 1) != 0) {
    result = CT_INSTANCE_PREPARE_BOOTSTRAP;
    goto done;
  }
  {
    int covered = ct_instance_path_inside(prepared->cwd_real, home_real);
    for (index = 0U; !covered && index < prepared->mount_count; ++index) {
      const struct ct_instance_mount *mount = &prepared->mounts[index];
      if (mount->generated || mount->profile_identity == NULL ||
          !ct_instance_path_inside(prepared->cwd_real, mount->source_real)) continue;
      {
        char mapped[CT_INSTANCE_PATH_MAX];
        if (snprintf(mapped, sizeof(mapped), "%s%s", mount->target,
                     prepared->cwd_real + strlen(mount->source_real)) >=
            (int)sizeof(mapped)) {
          result = CT_INSTANCE_PREPARE_CWD;
          goto done;
        }
        if (strcmp(mapped, prepared->cwd_real) == 0) covered = 1;
      }
    }
    if (!covered && ct_instance_mount_add(prepared, prepared->cwd_real,
                                          prepared->cwd_real,
                                          "persistent-automatic-cwd", "inherit",
                                          "runtime-default", 0, 1, 1) != 0) {
      result = CT_INSTANCE_PREPARE_CWD;
      goto done;
    }
  }
  for (index = 0U; index < prepared->mount_count; ++index) {
    if (!prepared->mounts[index].semantic) continue;
    entries[entry_count++] = (struct ct_mount_plan_entry){
        prepared->mounts[index].role, prepared->mounts[index].target,
        prepared->mounts[index].generated ? prepared->mounts[index].source_real :
                                            prepared->mounts[index].target,
        prepared->mounts[index].access,
        prepared->mounts[index].recursion};
  }
  if (ct_instance_state_root(state_root) != 0 ||
      ct_mount_plan_publish(&(struct ct_mount_plan){
                                request->backend, projection.strategy,
                                projection.completeness, projection.group_mode,
                                entries, entry_count},
                            state_root, prepared->manifest) != 0) {
    result = CT_INSTANCE_PREPARE_MOUNT_PLAN;
    goto done;
  }
  {
    const size_t semantic_count = prepared->mount_count;
    for (index = 0U; index < semantic_count; ++index) {
      if (ct_instance_add_state_mask(prepared,
                                     prepared->mounts[index].source_real,
                                     prepared->mounts[index].target,
                                     state_root) != 0) {
        result = CT_INSTANCE_PREPARE_MOUNT_PLAN;
        goto done;
      }
    }
    if (ct_instance_add_state_mask(prepared, home_real, home, state_root) != 0 ||
        ct_instance_add_state_mask(prepared, prepared->cwd_real, prepared->cwd,
                                   state_root) != 0 ||
        ct_instance_mount_add(prepared, prepared->manifest,
                              "/.container-tools-mount-plan", "manifest-internal",
                              "read-only", "runtime-default", 0, 0, 0) != 0) {
      result = CT_INSTANCE_PREPARE_MOUNT_PLAN;
      goto done;
    }
  }
  if (ct_instance_field_append(fields, &field_count, request->backend) != 0 ||
      (request->runtime_argument != NULL &&
       ct_instance_field_append(fields, &field_count,
                                request->runtime_argument) != 0) ||
      ct_instance_field_append(fields, &field_count, uid) != 0 ||
      ct_instance_field_append(fields, &field_count, host) != 0 ||
      ct_instance_field_append(fields, &field_count, home) != 0 ||
      ct_instance_field_append(fields, &field_count, request->root) != 0 ||
      ct_instance_field_append(fields, &field_count, prepared->image_real) != 0 ||
      ct_instance_field_append(fields, &field_count,
                               prepared->image_identity) != 0 ||
      ct_instance_field_append(fields, &field_count, bootstrap_identity) != 0 ||
      ct_instance_field_append(fields, &field_count,
                               "ct-instance-profile-v3") != 0 ||
      ct_instance_field_append(fields, &field_count,
                               "ct-host-projection-profile-v1") != 0) {
    goto done;
  }
  projection_fields[projection_count++] = "ct-host-projection-profile-v1";
  projection_fields[projection_count++] = "host-projection-v7";
  projection_fields[projection_count++] = projection.strategy;
  projection_fields[projection_count++] = "runtime-default";
  for (index = 0U; index < projection.entry_count; ++index) {
    projection_fields[projection_count++] = projection.strategy;
    projection_fields[projection_count++] = projection.entries[index].source;
    projection_fields[projection_count++] = projection.entries[index].target;
    projection_fields[projection_count++] = projection.entries[index].destination;
    projection_fields[projection_count++] = "writable";
    projection_fields[projection_count++] = "runtime-default";
  }
  if (ct_profile_digest_fields(projection_fields, projection_count,
                               projection_digest) != 0) {
    goto done;
  }
  if (ct_instance_field_append(fields, &field_count, projection_digest) != 0 ||
      ct_instance_field_append(fields, &field_count, uid) != 0 ||
      ct_instance_field_append(fields, &field_count, gid) != 0 ||
      ct_instance_field_append(fields, &field_count, projection.group_mode) != 0 ||
      ct_instance_groups(fields, &field_count, group_values,
                          CT_INSTANCE_GROUP_MAX) != 0) {
    goto done;
  }
  for (index = 0U; index < prepared->mount_count; ++index) {
    if (ct_instance_field_append(
            fields, &field_count,
            prepared->mounts[index].generated ? "--mount" : "--bind") != 0 ||
        ct_instance_field_append(fields, &field_count,
                                 prepared->mounts[index].descriptor) != 0) {
      goto done;
    }
  }
  for (index = 0U; index < prepared->mount_count; ++index) {
    if (prepared->mounts[index].profile_identity != NULL) {
      if (ct_instance_field_append(fields, &field_count,
                                   prepared->mounts[index].profile_identity) != 0) {
        goto done;
      }
    }
  }
  if (ct_profile_digest_fields(fields, field_count, prepared->digest) != 0 ||
      snprintf(prepared->name, sizeof(prepared->name), "mkchad-%.32s",
               prepared->digest) >= (int)sizeof(prepared->name)) goto done;
  if (!ct_instance_assets_current(request, prepared, bootstrap_real,
                                  bootstrap_stat)) {
    result = CT_INSTANCE_PREPARE_ASSETS;
    goto done;
  }
  result = CT_INSTANCE_PREPARE_OK;
done:
  free(entries);
  free(fields);
  free(projection_fields);
  free(group_values);
  if (result != CT_INSTANCE_PREPARE_OK) ct_instance_prepared_free(prepared);
  return result;
}

static void ct_instance_mount_config_diagnostic(
    enum ct_mount_environment_status status)
{
  const char *configured = getenv("CT_MOUNT_CFG");
  const bool has_config_override = configured != NULL && configured[0] != '\0';
  switch (status) {
    case CT_MOUNT_ENVIRONMENT_CONFIG_IO:
      ct_instance_diagnostic(
          "mount-configuration",
          has_config_override ?
              "make the file selected by CT_MOUNT_CFG readable, then retry" :
              "make $HOME/.config/ct_mount.conf readable, then retry");
      return;
    case CT_MOUNT_ENVIRONMENT_CONFIG_OPTIONS:
      ct_instance_diagnostic(
          "mount-configuration",
          has_config_override ?
              "the file selected by CT_MOUNT_CFG contains unsupported or malformed mount detector options; use only --exclude-fs, --exclude-path, and --add-path, then retry" :
              "$HOME/.config/ct_mount.conf contains unsupported or malformed mount detector options; use only --exclude-fs, --exclude-path, and --add-path, then retry");
      return;
    case CT_MOUNT_ENVIRONMENT_CONFIG_ADD_PATH:
      ct_instance_diagnostic(
          "mount-configuration",
          has_config_override ?
              "the file selected by CT_MOUNT_CFG contains an --add-path value that is not an absolute same-path mount; remove the remap from this file or pass --ct-bind HOST:CONTAINER to the container launcher instead, then retry" :
              "$HOME/.config/ct_mount.conf contains an --add-path value that is not an absolute same-path mount; remove the remap from this file or pass --ct-bind HOST:CONTAINER to the container launcher instead, then retry");
      return;
    case CT_MOUNT_ENVIRONMENT_CONFIG_CT_BIND:
      ct_instance_diagnostic(
          "mount-configuration",
          has_config_override ?
              "the file selected by CT_MOUNT_CFG contains --ct-bind, which is a container launcher option rather than a mount detector option; remove it from this file and pass the bind to the container launcher, then retry" :
              "$HOME/.config/ct_mount.conf contains --ct-bind, which is a container launcher option rather than a mount detector option; remove it from this file and pass the bind to the container launcher, then retry");
      return;
    case CT_MOUNT_ENVIRONMENT_EXTRA_OPTIONS:
      ct_instance_diagnostic(
          "mount-configuration",
          "MOUNT_DETECTOR_ARGS contains unsupported or malformed mount detector options; use only --exclude-fs, --exclude-path, and --add-path, then retry");
      return;
    case CT_MOUNT_ENVIRONMENT_EXTRA_ADD_PATH:
      ct_instance_diagnostic(
          "mount-configuration",
          "MOUNT_DETECTOR_ARGS contains an --add-path value that is not an absolute same-path mount; remove the remap from MOUNT_DETECTOR_ARGS or pass --ct-bind HOST:CONTAINER to the container launcher instead, then retry");
      return;
    case CT_MOUNT_ENVIRONMENT_EXTRA_CT_BIND:
      ct_instance_diagnostic(
          "mount-configuration",
          "MOUNT_DETECTOR_ARGS contains --ct-bind, which is a container launcher option rather than a mount detector option; remove it from MOUNT_DETECTOR_ARGS and pass the bind to the container launcher, then retry");
      return;
    case CT_MOUNT_ENVIRONMENT_COMBINED_OPTIONS:
      ct_instance_diagnostic(
          "mount-configuration",
          "the combined mount configuration file and MOUNT_DETECTOR_ARGS exceed supported limits; reduce the options, then retry");
      return;
    case CT_MOUNT_ENVIRONMENT_DISCOVERY:
      ct_instance_diagnostic(
          "mount-configuration",
          "repair /proc/mounts access or reduce the configured mount set, then retry");
      return;
    case CT_MOUNT_ENVIRONMENT_DETECTED_PATH:
      ct_instance_diagnostic(
          "mount-configuration",
          "mount detection produced a path that is not an absolute same-path mount; adjust the detector exclusions, then retry");
      return;
    case CT_MOUNT_ENVIRONMENT_OK:
    case CT_MOUNT_ENVIRONMENT_INTERNAL:
    default:
      ct_instance_diagnostic(
          "mount-configuration",
          "retry; if this persists, reinstall container-tools and report the failure");
  }
}

static void ct_instance_prepare_diagnostic(
    enum ct_instance_prepare_status status,
    const struct ct_instance_prepared *prepared)
{
  switch (status) {
    case CT_INSTANCE_PREPARE_IMAGE:
      ct_instance_diagnostic("image", "make the container image exist as a readable regular file, then retry");
      return;
    case CT_INSTANCE_PREPARE_BOOTSTRAP:
      ct_instance_diagnostic("bootstrap", "make the bootstrap file exist as a readable regular file, then retry");
      return;
    case CT_INSTANCE_PREPARE_HOME:
      ct_instance_diagnostic("home", "set HOME to an accessible absolute directory, then retry");
      return;
    case CT_INSTANCE_PREPARE_CWD:
      ct_instance_diagnostic("working-directory", "run from an accessible working directory, then retry");
      return;
    case CT_INSTANCE_PREPARE_PROJECTION:
      ct_instance_diagnostic("host-projection", "repair local runtime projection support or use --ct-host-root auto, then retry");
      return;
    case CT_INSTANCE_PREPARE_MOUNT_CONFIG:
      ct_instance_mount_config_diagnostic(prepared->mount_environment_status);
      return;
    case CT_INSTANCE_PREPARE_BIND:
      ct_instance_diagnostic("bind-source", "make every requested or detected bind source accessible, then retry");
      return;
    case CT_INSTANCE_PREPARE_MOUNT_PLAN:
      ct_instance_diagnostic("mount-plan", "make persistent mount-plan state privately writable, then retry");
      return;
    case CT_INSTANCE_PREPARE_ASSETS:
      ct_instance_diagnostic("assets", "restore stable image and bootstrap assets, then retry");
      return;
    case CT_INSTANCE_PREPARE_INTERNAL:
    default:
      ct_instance_diagnostic("profile-finalization", "retry; if this persists, reinstall container-tools and report the failure");
  }
}

static size_t ct_instance_runtime_prefix(const struct ct_instance_request *request,
                                          char *arguments[])
{
  size_t count = 0U;
  arguments[count++] = (char *)request->backend;
  if (request->runtime_argument != NULL) {
    arguments[count++] = (char *)request->runtime_argument;
  }
  return count;
}

static int ct_instance_interrupted(int status)
{
  return status == 128 + SIGINT || status == 128 + SIGTERM;
}

static int ct_instance_probe(const struct ct_instance_request *request,
                             const char *name, const char *digest,
                             const char *nonce)
{
  char uri[64];
  char *command[16];
  size_t count = ct_instance_runtime_prefix(request, command);
  if (snprintf(uri, sizeof(uri), "instance://%s", name) >= (int)sizeof(uri)) return 125;
  command[count++] = "exec";
  command[count++] = uri;
  command[count++] = "/bin/sh";
  command[count++] = "-c";
  command[count++] = "exec 3</.container-tools-instance-identity || exit 42; IFS= read -r n <&3 || exit 42; IFS= read -r p <&3 || exit 42; IFS= read -r x <&3 || exit 42; [ \"$n\" = \"$1\" ] && [ \"$p\" = \"$2\" ] && { [ -z \"${3:-}\" ] || [ \"$x\" = \"$3\" ]; }";
  command[count++] = "sh";
  command[count++] = (char *)name;
  command[count++] = (char *)digest;
  command[count++] = (char *)(nonce == NULL ? "" : nonce);
  command[count] = NULL;
  return ct_process_run_operation_quiet(command, "instance-probe");
}

static int ct_instance_absent(const struct ct_instance_request *request,
                              const char *name)
{
  char output[4096];
  char *command[8];
  size_t count = ct_instance_runtime_prefix(request, command);
  yyjson_doc *document;
  yyjson_val *root;
  int empty = 0;

  command[count++] = "instance";
  command[count++] = "list";
  command[count++] = "--json";
  command[count++] = (char *)name;
  command[count] = NULL;
  if (ct_process_run_operation_capture_quiet(
          command, "instance-probe", output, sizeof(output)) != 0) return 0;
  document = yyjson_read(output, strlen(output), 0);
  if (document == NULL) return 0;
  root = yyjson_doc_get_root(document);
  if (yyjson_is_arr(root)) {
    empty = yyjson_arr_size(root) == 0U;
  } else if (yyjson_is_obj(root) && yyjson_obj_size(root) == 1U) {
    yyjson_val *instances = yyjson_obj_get(root, "instances");
    empty = yyjson_is_arr(instances) && yyjson_arr_size(instances) == 0U;
  }
  yyjson_doc_free(document);
  return empty;
}

static int ct_instance_nonce(char output[33])
{
  const char *configured = getenv("CT_INSTANCE_CREATION_NONCE");
  int descriptor;
  unsigned char bytes[16];
  size_t used = 0U, index;
  if (configured != NULL && configured[0] != '\0') {
    if (strlen(configured) != 32U || strspn(configured, "0123456789abcdef") != 32U) return 1;
    (void)snprintf(output, 33U, "%s", configured);
    return 0;
  }
  descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) return 1;
  while (used < sizeof(bytes)) {
    const ssize_t received = read(descriptor, bytes + used, sizeof(bytes) - used);
    if (received <= 0) { (void)close(descriptor); return 1; }
    used += (size_t)received;
  }
  if (close(descriptor) != 0) return 1;
  for (index = 0U; index < sizeof(bytes); ++index) {
    (void)snprintf(output + index * 2U, 3U, "%02x", bytes[index]);
  }
  return 0;
}

static int ct_instance_start(const struct ct_instance_request *request,
                             const struct ct_instance_prepared *prepared,
                             const char *pending_path)
{
  char identity_bind[CT_INSTANCE_DESCRIPTOR_MAX];
  char profile_environment[160], user_environment[320];
  char **command = calloc(CT_INSTANCE_ARGUMENT_MAX, sizeof(*command));
  size_t count, index;
  int result;
  if (command == NULL) return 125;
  if (snprintf(identity_bind, sizeof(identity_bind),
               "%s:/.container-tools-instance-identity:ro", pending_path) >=
      (int)sizeof(identity_bind)) { free(command); return 125; }
  if (snprintf(profile_environment, sizeof(profile_environment),
               "SINGULARITYENV_CONTAINER_TOOLS_PROFILE=%s", prepared->digest) >=
      (int)sizeof(profile_environment)) { free(command); return 125; }
  if (snprintf(user_environment, sizeof(user_environment), "SINGULARITYENV_USER=%s",
               prepared->user) >= (int)sizeof(user_environment)) {
    free(command);
    return 125;
  }
  count = ct_instance_runtime_prefix(request, command);
  command[count++] = "instance";
  command[count++] = "start";
  for (index = 0U; index < prepared->mount_count; ++index) {
    command[count++] = prepared->mounts[index].generated ? "--mount" : "--bind";
    command[count++] = prepared->mounts[index].descriptor;
  }
  command[count++] = "--bind";
  command[count++] = identity_bind;
  command[count++] = "--env";
  command[count++] = user_environment;
  command[count++] = "--env";
  command[count++] = profile_environment;
  for (index = 0U; index < request->environment_count; ++index) {
    command[count++] = "--env";
    command[count++] = (char *)request->environment[index];
  }
  command[count++] = (char *)prepared->image_real;
  command[count++] = (char *)prepared->name;
  command[count] = NULL;
  result = ct_process_run_operation_stdout_to_stderr(command, "instance-start");
  free(command);
  return result;
}

static void ct_instance_print_dry(char *const arguments[])
{
  size_t index;
  for (index = 0U; arguments[index] != NULL; ++index) {
    const char *cursor;
    for (cursor = arguments[index]; *cursor != '\0'; ++cursor) {
      if (strchr(" \\;\t\n", *cursor) != NULL) (void)fputc('\\', stdout);
      (void)fputc(*cursor, stdout);
    }
    (void)fputc(' ', stdout);
  }
  (void)fputc('\n', stdout);
}

static int ct_instance_payload(const struct ct_instance_request *request,
                               const struct ct_instance_prepared *prepared,
                               const char *uri, int dry)
{
  char profile_environment[160], user_environment[320];
  char **command = calloc(CT_INSTANCE_ARGUMENT_MAX, sizeof(*command));
  size_t count, index;
  int result;
  if (command == NULL ||
      snprintf(profile_environment, sizeof(profile_environment),
               "SINGULARITYENV_CONTAINER_TOOLS_PROFILE=%s",
               dry ? "dry-run" : prepared->digest) >= (int)sizeof(profile_environment) ||
      snprintf(user_environment, sizeof(user_environment), "SINGULARITYENV_USER=%s",
               prepared->user) >= (int)sizeof(user_environment)) {
    free(command);
    return 125;
  }
  count = ct_instance_runtime_prefix(request, command);
  command[count++] = "exec";
  command[count++] = "--pwd";
  command[count++] = (char *)prepared->cwd;
  command[count++] = "--env";
  command[count++] = user_environment;
  if (!dry) {
    command[count++] = "--env";
    command[count++] = profile_environment;
  }
  for (index = 0U; index < request->environment_count; ++index) {
    command[count++] = "--env";
    command[count++] = (char *)request->environment[index];
  }
  command[count++] = (char *)uri;
  if (request->bootstrap != NULL) command[count++] = "/.container-tools-bootstrap";
  for (index = 1U; index < request->payload_count; ++index) {
    command[count++] = request->payload[index];
  }
  command[count] = NULL;
  if (dry) {
    ct_instance_print_dry(command);
    result = 0;
  } else {
    result = ct_backend_outer_dispatch(command, count);
  }
  free(command);
  return result;
}

static enum ct_instance_prepare_status ct_instance_prepare_dry(
    const struct ct_instance_request *request, struct ct_instance_prepared *prepared)
{
  memset(prepared, 0, sizeof(*prepared));
  if (getcwd(prepared->cwd, sizeof(prepared->cwd)) == NULL) return CT_INSTANCE_PREPARE_CWD;
  if (ct_instance_user(prepared->user) != 0) return CT_INSTANCE_PREPARE_INTERNAL;
  if (!ct_instance_regular_file(request->image)) return CT_INSTANCE_PREPARE_IMAGE;
  return CT_INSTANCE_PREPARE_OK;
}

int ct_instance_command(int argument_count, char *const arguments[], int identity_only)
{
  struct ct_instance_request request;
  struct ct_instance_prepared prepared;
  struct ct_runtime_config config;
  struct ct_state_pending pending;
  char pending_path[CT_STATE_PATH_MAX], nonce[33] = "", uri[64];
  char bootstrap_real[CT_INSTANCE_PATH_MAX] = "", bootstrap_identity[256] = "";
  int lock = -1, probe, ready = 0, pending_restart = 0, json = 0, result = 1;
  int argument_index;
  const char *dry_run;
  struct stat pending_status;
  enum ct_instance_prepare_status prepare_status;
  enum ct_state_lock_status lock_status;

  memset(&request, 0, sizeof(request));
  if (identity_only != 0 && argument_count > 0 &&
      strcmp(arguments[argument_count - 1], "--json") == 0) {
    json = 1;
    --argument_count;
  }
  if (argument_count >= 3 && strcmp(arguments[1], "--ct-instance-root") == 0 &&
      !ct_instance_path_scalar(arguments[2])) {
    ct_cli_diagnostic("persistent instance request", "usage",
                      "use an absolute instance root without ':', ',' or newlines, then retry");
    return 1;
  }
  for (argument_index = 3; argument_index + 1 < argument_count; ++argument_index) {
    if (strcmp(arguments[argument_index], "--") == 0) break;
    if (strcmp(arguments[argument_index], "--ct-bind") == 0) {
      const char *separator = strchr(arguments[argument_index + 1], ':');
      char target[CT_INSTANCE_PATH_MAX];
      if (separator != NULL && ct_instance_normalize_path(separator + 1, target) == 0) {
        if (strcmp(target, "/.container-tools-instance-identity") == 0) {
          ct_cli_diagnostic("persistent instance request", "reserved-bind",
                            "choose a destination other than protected persistent-instance paths, then retry");
          return 1;
        }
        if (strcmp(target, "/.container-tools-mount-plan") == 0 ||
            strncmp(target, "/.container-tools-mount-plan/", 30U) == 0) {
          ct_cli_diagnostic("persistent instance request", "reserved-bind",
                            "choose a destination other than protected persistent-instance paths, then retry");
          return 1;
        }
      }
      ++argument_index;
    }
  }
  if (ct_instance_parse(argument_count, arguments, &request) != 0) {
    ct_cli_diagnostic("persistent instance request", "usage",
                      "use 'container-tools instance exec --help' for valid syntax");
    return 1;
  }
  if (ct_runtime_config_load_environment(&config) != CT_CONFIG_OK ||
      ct_storage_select_runtime(request.backend, &config) != 0) return 1;
  if (
      unsetenv("SINGULARITY_BIND") != 0 || unsetenv("SINGULARITY_BINDPATH") != 0 ||
      unsetenv("SINGULARITY_MOUNT") != 0 || unsetenv("APPTAINER_BIND") != 0 ||
      unsetenv("APPTAINER_BINDPATH") != 0 || unsetenv("APPTAINER_MOUNT") != 0) {
    ct_instance_diagnostic("runtime-environment",
                           "repair the inherited runtime environment and retry");
    return 1;
  }
  dry_run = getenv("CT_DRY_RUN");
  if (identity_only == 0 && dry_run != NULL && dry_run[0] != '\0') {
    prepare_status = ct_instance_prepare_dry(&request, &prepared);
    if (prepare_status != CT_INSTANCE_PREPARE_OK) {
      ct_instance_prepare_diagnostic(prepare_status, &prepared);
      return 1;
    }
    return ct_instance_payload(&request, &prepared, "instance://dry-run", 1);
  }
  prepare_status = ct_instance_prepare(&request, &prepared);
  if (prepare_status != CT_INSTANCE_PREPARE_OK) {
    ct_instance_prepare_diagnostic(prepare_status, &prepared);
    return 1;
  }
  if (identity_only != 0) {
    if (json) {
      (void)fprintf(stdout,
                    "{\"schema\":\"container-tools.instance-identity/v1\",\"identity\":\"%s\"}\n",
                    prepared.digest);
    } else {
      (void)fprintf(stdout, "%s\n", prepared.digest);
    }
    ct_instance_prepared_free(&prepared);
    return 0;
  }
  if (request.bootstrap != NULL &&
      (realpath(request.bootstrap, bootstrap_real) == NULL ||
       ct_instance_stat_identity(bootstrap_real, 1, bootstrap_identity) != 0)) {
    ct_cli_diagnostic("persistent instance assets", "changed",
                      "restore stable image and bootstrap assets, then retry");
    goto done;
  }
  lock_status = ct_state_lock(request.root, prepared.name, &lock);
  if (lock_status != CT_STATE_LOCK_OK) {
    if (lock_status == CT_STATE_LOCK_TIMEOUT) {
      ct_cli_diagnostic("persistent instance lock", "contention",
                        "wait for the concurrent instance creation to finish, then retry");
    } else {
      ct_cli_diagnostic("persistent instance state", "setup",
                        "make the persistent instance state directory privately writable, then retry");
    }
    goto done;
  }
  if (ct_state_pending_path(request.root, prepared.name, pending_path) != 0) {
    ct_cli_diagnostic("persistent instance state", "setup",
                      "make the persistent instance state directory usable, then retry");
    goto done;
  }
  errno = 0;
  if (ct_storage_timeout_lstat(pending_path, &pending_status) == 0) {
    if (ct_state_pending_read(pending_path, prepared.name, prepared.digest,
                              &pending) != 0) {
      ct_instance_recovery_diagnostic("pending-journal",
                                      "repair or remove the malformed private pending journal, then retry");
      goto done;
    }
    memcpy(nonce, pending.nonce, sizeof(nonce));
    probe = ct_instance_probe(&request, prepared.name, prepared.digest, nonce);
    if (ct_instance_interrupted(probe)) {
      result = probe;
      goto done;
    }
    if (probe == 0) {
      if (ct_state_pending_clear(pending_path) != 0) {
        ct_instance_recovery_diagnostic("pending-journal-update",
                                        "repair private instance state storage and retry");
        goto done;
      }
      ready = 1;
    } else if (probe == 42) {
      const int profile_probe = ct_instance_probe(
          &request, prepared.name, prepared.digest, NULL);
      if (ct_instance_interrupted(profile_probe)) {
        result = profile_probe;
        goto done;
      }
      if (profile_probe == 0) {
        ct_instance_recovery_diagnostic("pending-nonce-mismatch",
                                        "wait for the original creator to finish or use a fresh instance root, then retry");
      } else {
        ct_instance_recovery_diagnostic("profile-mismatch",
                                        "use consistent image, bootstrap, bind, and environment inputs, then retry");
      }
      goto done;
    } else if (!ct_instance_absent(&request, prepared.name)) {
      ct_instance_recovery_diagnostic("pending-journal-recovery",
                                      "verify the backend can list instances, then retry without removing the pending journal");
      goto done;
    } else {
      pending_restart = 1;
    }
  } else if (errno != ENOENT) {
    ct_instance_recovery_diagnostic("pending-journal-recovery",
                                    "repair access to the private pending journal and retry");
    goto done;
  }
  if (!ready && !pending_restart) {
    probe = ct_instance_probe(&request, prepared.name, prepared.digest, NULL);
    if (ct_instance_interrupted(probe)) {
      result = probe;
      goto done;
    }
    if (probe == 0) {
      ready = 1;
    } else if (probe == 42) {
      ct_instance_recovery_diagnostic("profile-mismatch",
                                      "use consistent image, bootstrap, bind, and environment inputs, then retry");
      goto done;
    } else if (probe == 124 || probe == 137) {
      ct_cli_diagnostic("persistent instance liveness", "timeout",
                        "wait for the backend to become responsive, then retry");
      goto done;
    } else if (!ct_instance_absent(&request, prepared.name)) {
      ct_cli_diagnostic("persistent instance liveness", "recovery",
                        "verify the backend can probe and list instances, then retry");
      goto done;
    }
  }
  if (!ready) {
    int start_result;
    if (!pending_restart) {
      if (ct_instance_nonce(nonce) != 0 ||
          ct_state_pending_write(pending_path, prepared.name, prepared.digest,
                                 nonce) != 0) {
        ct_instance_recovery_diagnostic("pending-journal-update",
                                        "repair private instance state storage and retry");
        goto done;
      }
    }
    start_result = ct_instance_start(&request, &prepared, pending_path);
    if (ct_instance_interrupted(start_result)) {
      result = start_result;
      goto done;
    }
    probe = ct_instance_probe(&request, prepared.name, prepared.digest, nonce);
    if (ct_instance_interrupted(probe)) {
      result = probe;
      goto done;
    }
    if (probe != 0) {
      if (probe == 42) {
        ct_instance_recovery_diagnostic("profile-mismatch",
                                        "use consistent image, bootstrap, bind, and environment inputs, then retry");
      } else if (start_result == 124) {
        ct_cli_diagnostic("persistent instance backend-start", "timeout",
                          "wait for the backend start to finish, then retry");
      } else if (start_result != 0) {
        ct_cli_diagnostic("persistent instance backend-start", "failed",
                          "verify the backend installation and image availability, then retry");
      } else if (probe == 124 || probe == 137) {
        ct_cli_diagnostic("persistent instance backend-start", "timeout",
                          "wait for the started instance to become responsive, then retry");
      } else {
        ct_cli_diagnostic("persistent instance backend-start", "failed",
                          "verify the backend can start and probe instances, then retry");
      }
      goto done;
    }
    if (ct_state_pending_clear(pending_path) != 0) {
      ct_instance_recovery_diagnostic("pending-journal-update",
                                      "repair private instance state storage and retry");
      goto done;
    }
  }
  if (!ct_instance_assets_current(&request, &prepared, bootstrap_real,
                                  bootstrap_identity)) {
    ct_cli_diagnostic("persistent instance assets", "changed",
                      "restore stable image and bootstrap assets, then retry");
    goto done;
  }
  (void)ct_state_identity_write(request.root, prepared.name, prepared.image_real,
                                prepared.image_identity, prepared.digest);
  ct_state_unlock(lock);
  lock = -1;
  if (snprintf(uri, sizeof(uri), "instance://%s", prepared.name) >=
      (int)sizeof(uri)) goto done;
  result = ct_instance_payload(&request, &prepared, uri, 0);
done:
  ct_state_unlock(lock);
  ct_instance_prepared_free(&prepared);
  return result;
}
