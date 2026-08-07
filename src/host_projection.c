/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "host_projection.h"

#include "backend_outer.h"
#include "sha256.h"
#include "storage.h"
#include "storage_timeout.h"
#include "process.h"

#include "yyjson.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

extern char *realpath(const char *restrict path, char *restrict resolved_path);

#define CT_HOST_CACHE_MAX_BYTES 1048576U
#define CT_HOST_CACHE_MAX_PAIRS 4096U
#define CT_HOST_PATH_MAX 4096U
#define CT_HOST_PROBE_MAX_ARGS (CT_HOST_PROJECTION_MAX_ENTRIES * 2U + 16U)
#define CT_HOST_FALLBACK_MAX_VISITED 4096U
#define CT_HOST_CACHE_MAX_TEMPORARIES 32U

static unsigned long ct_cache_temporary_sequence;

struct ct_mountinfo { char path[CT_HOST_PATH_MAX]; char filesystem[32]; };

static int ct_unix_endpoint(const char *endpoint)
{
  return endpoint == NULL || endpoint[0] == '\0' || strncmp(endpoint, "unix://", 7U) == 0 || strncmp(endpoint, "unix:", 5U) == 0;
}

/* Read only one documented JSON selector.  A malformed or unsafe client file
 * cannot silently fall back to a local daemon assumption. */
static int ct_client_selector(const char *file, const char *field, char selector[128], char identity[65])
{
  struct stat status, descriptor_status, final_status;
  struct ct_sha256 hash;
  unsigned char raw[32];
  char *data;
  size_t used = 0U;
  int descriptor;
  ssize_t received = 0;
  yyjson_doc *document;
  yyjson_val *value;
  const char *string;

  selector[0] = '\0';
  if (file == NULL || file[0] == '\0' || ct_storage_timeout_lstat(file, &status) != 0) {
    if (errno == ENOENT) { (void)snprintf(identity, 65U, "absent"); return 0; }
    return 1;
  }
  if (!S_ISREG(status.st_mode) || S_ISLNK(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & 022U) != 0U || status.st_size < 0 || (size_t)status.st_size > CT_HOST_CACHE_MAX_BYTES) return 1;
  data = malloc((size_t)status.st_size + 1U);
  if (data == NULL) return 1;
  descriptor = ct_storage_timeout_open(file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0);
  if (descriptor < 0 || ct_storage_timeout_fstat(descriptor, &descriptor_status) != 0 ||
      !S_ISREG(descriptor_status.st_mode) || descriptor_status.st_uid != status.st_uid ||
      descriptor_status.st_dev != status.st_dev || descriptor_status.st_ino != status.st_ino ||
      descriptor_status.st_size != status.st_size) {
    if (descriptor >= 0) (void)ct_storage_timeout_close(descriptor);
    free(data);
    return 1;
  }
  while (used < (size_t)status.st_size && (received = ct_storage_timeout_read(descriptor, data + used, (size_t)status.st_size - used)) > 0) used += (size_t)received;
  if (ct_storage_timeout_close(descriptor) != 0 || received < 0 || used != (size_t)status.st_size ||
      ct_storage_timeout_lstat(file, &final_status) != 0 || !S_ISREG(final_status.st_mode) ||
      S_ISLNK(final_status.st_mode) || final_status.st_uid != status.st_uid ||
      final_status.st_dev != status.st_dev || final_status.st_ino != status.st_ino ||
      final_status.st_size != status.st_size) { free(data); return 1; }
  data[used] = '\0';
  ct_sha256_init(&hash); ct_sha256_update(&hash, data, used); ct_sha256_final(&hash, raw); ct_sha256_hex(raw, identity);
  document = yyjson_read(data, used, 0U);
  if (document == NULL || !yyjson_is_obj(yyjson_doc_get_root(document))) { yyjson_doc_free(document); free(data); return 1; }
  value = yyjson_obj_get(yyjson_doc_get_root(document), field);
  if (value != NULL) {
    string = yyjson_get_str(value);
    if (string == NULL || snprintf(selector, 128U, "%s", string) >= 128) { yyjson_doc_free(document); free(data); return 1; }
  }
  yyjson_doc_free(document);
  free(data);
  return 0;
}

static int ct_host_projection_selector(const char *backend, char output[512])
{
  const char *endpoint;
  const char *home = getenv("HOME");
  const char *config_home = getenv("XDG_CONFIG_HOME");
  char config[CT_HOST_PATH_MAX], selector[128], identity[65];

  if (backend == NULL) return 1;
  if (strcmp(backend, "singularity") == 0 || strcmp(backend, "apptainer") == 0) return snprintf(output, 512U, "local") >= 512;
  if (strcmp(backend, "docker") == 0) {
    const char *context = getenv("DOCKER_CONTEXT");
    const char *machine = getenv("DOCKER_MACHINE_NAME");
    endpoint = getenv("DOCKER_HOST");
    if (context != NULL && context[0] == '\0') context = NULL;
    if (context == NULL && snprintf(config, sizeof(config), "%s%s", getenv("DOCKER_CONFIG") == NULL ? (home == NULL ? "" : home) : getenv("DOCKER_CONFIG"), getenv("DOCKER_CONFIG") == NULL ? "/.docker/config.json" : "/config.json") >= (int)sizeof(config)) return 1;
    if (context == NULL && ct_client_selector(config, "currentContext", selector, identity) != 0) return 1;
    if ((context != NULL && context[0] != '\0' && strcmp(context, "default") != 0) ||
        (context == NULL && selector[0] != '\0' && strcmp(selector, "default") != 0) ||
        (machine != NULL && machine[0] != '\0') || !ct_unix_endpoint(endpoint)) return 1;
    return snprintf(output, 512U, "docker:context=%s:config=%s:host=%s:machine=%s",
                    context == NULL ? (selector[0] == '\0' ? "default" : selector) : context,
                    context == NULL ? identity : "explicit", endpoint == NULL ? "" : endpoint,
                    machine == NULL ? "" : machine) >= 512;
  }
  if (strcmp(backend, "podman") == 0) {
    const char *connection = getenv("CONTAINER_CONNECTION");
    if (connection == NULL || connection[0] == '\0') connection = getenv("PODMAN_CONNECTION");
    if (connection != NULL && connection[0] == '\0') connection = NULL;
    endpoint = getenv("CONTAINER_HOST");
    if (endpoint == NULL || endpoint[0] == '\0') endpoint = getenv("DOCKER_HOST");
    if (connection == NULL && snprintf(config, sizeof(config), "%s/containers/podman-connections.json",
                                       config_home == NULL || config_home[0] == '\0' ? (home == NULL ? "" : home) : config_home) >= (int)sizeof(config)) return 1;
    if (connection == NULL && (config_home == NULL || config_home[0] == '\0') && home != NULL &&
        snprintf(config, sizeof(config), "%s/.config/containers/podman-connections.json", home) >= (int)sizeof(config)) return 1;
    if (connection == NULL && ct_client_selector(config, "Default", selector, identity) != 0) return 1;
    if ((connection != NULL && connection[0] != '\0') || (connection == NULL && selector[0] != '\0') || !ct_unix_endpoint(endpoint)) return 1;
    return snprintf(output, 512U, "podman:connection=%s:config=%s:host=%s",
                    connection == NULL ? "default" : connection, connection == NULL ? identity : "explicit",
                    endpoint == NULL ? "" : endpoint) >= 512;
  }
  return 1;
}

int ct_host_projection_endpoint_is_local(const char *backend)
{
  char selector[512];
  return ct_host_projection_selector(backend, selector) == 0;
}

static int ct_kernel_filesystem(const char *filesystem)
{
  static const char *const values[] = {"proc", "sysfs", "devtmpfs", "devpts", "securityfs", "cgroup", "cgroup2", "pstore", "efivarfs", "debugfs", "tracefs", "configfs", "fusectl", "mqueue", "hugetlbfs", "nsfs"};
  size_t index;
  if (filesystem == NULL) return 0;
  for (index = 0U; index < sizeof(values) / sizeof(values[0]); ++index) if (strcmp(filesystem, values[index]) == 0) return 1;
  return 0;
}

static int ct_root_relative(const char *path, const char *root, const char **relative)
{
  if (strcmp(root, "/") == 0) { *relative = path; return 0; }
  if (strcmp(path, root) == 0) { *relative = "/"; return 0; }
  if (strncmp(path, root, strlen(root)) == 0 && path[strlen(root)] == '/') { *relative = path + strlen(root); return 0; }
  return 1;
}

int ct_host_projection_source_is_eligible(const char *path, const char *filesystem)
{
  const char *root = getenv("CT_HOST_PROJECTION_SOURCE_ROOT");
  const char *relative;
  if (root == NULL || root[0] == '\0') root = "/";
  if (path == NULL || path[0] != '/' || ct_root_relative(path, root, &relative) != 0) return 0;
  if (strcmp(relative, "/host") == 0 || strncmp(relative, "/host/", 6U) == 0 || strcmp(relative, "/proc") == 0 || strncmp(relative, "/proc/", 6U) == 0 ||
      strcmp(relative, "/sys") == 0 || strncmp(relative, "/sys/", 5U) == 0 || strcmp(relative, "/dev") == 0 || strncmp(relative, "/dev/", 5U) == 0) return 0;
  return ct_kernel_filesystem(filesystem) == 0;
}

static int ct_copy(char destination[CT_HOST_PATH_MAX], const char *source)
{
  const int written = snprintf(destination, CT_HOST_PATH_MAX, "%s", source);
  return written < 0 || (size_t)written >= CT_HOST_PATH_MAX;
}

static int ct_unescape_mountinfo(char *value)
{
  char result[CT_HOST_PATH_MAX];
  size_t input = 0U, output = 0U;
  while (value[input] != '\0') {
    if (value[input] == '\\' && value[input + 1U] >= '0' && value[input + 1U] <= '7' && value[input + 2U] >= '0' && value[input + 2U] <= '7' && value[input + 3U] >= '0' && value[input + 3U] <= '7') {
      const unsigned int byte = (unsigned int)(value[input + 1U] - '0') * 64U + (unsigned int)(value[input + 2U] - '0') * 8U + (unsigned int)(value[input + 3U] - '0');
      if (byte == 0U || output + 1U >= sizeof(result)) return 1;
      result[output++] = (char)byte; input += 4U;
    } else {
      if (output + 1U >= sizeof(result)) return 1;
      result[output++] = value[input++];
    }
  }
  result[output] = '\0';
  return ct_copy(value, result);
}

static int ct_mountinfo_read(struct ct_mountinfo mounts[CT_HOST_CACHE_MAX_PAIRS], size_t *count)
{
  const char *file = getenv("CT_HOST_PROJECTION_MOUNTINFO");
  const char *root = getenv("CT_HOST_PROJECTION_SOURCE_ROOT");
  char buffer[CT_HOST_CACHE_MAX_BYTES + 1U], *line, *save;
  int descriptor;
  ssize_t bytes = 0;
  size_t used = 0U;
  if (file == NULL || file[0] == '\0') file = "/proc/self/mountinfo";
  if (root == NULL || root[0] == '\0') root = "/";
  descriptor = ct_storage_timeout_open(file, O_RDONLY | O_CLOEXEC, 0);
  if (descriptor < 0) return 1;
  while (used < CT_HOST_CACHE_MAX_BYTES && (bytes = ct_storage_timeout_read(descriptor, buffer + used, CT_HOST_CACHE_MAX_BYTES - used)) > 0) used += (size_t)bytes;
  if (ct_storage_timeout_close(descriptor) != 0 || bytes < 0 || used == CT_HOST_CACHE_MAX_BYTES) return 1;
  buffer[used] = '\0'; *count = 0U;
  for (line = strtok_r(buffer, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
    char *tokens[256], *token_save, *token;
    size_t tokens_count = 0U, dash = 0U;
    token = strtok_r(line, " ", &token_save);
    while (token != NULL && tokens_count < sizeof(tokens) / sizeof(tokens[0])) { tokens[tokens_count++] = token; token = strtok_r(NULL, " ", &token_save); }
    while (dash < tokens_count && strcmp(tokens[dash], "-") != 0) ++dash;
    if (tokens_count < 7U || dash + 1U >= tokens_count || tokens[4][0] != '/' || *count == CT_HOST_CACHE_MAX_PAIRS) continue;
    if (ct_unescape_mountinfo(tokens[4]) != 0) return 1;
    if (strcmp(root, "/") != 0 && strcmp(tokens[4], "/") == 0) {
      if (ct_copy(mounts[*count].path, root) != 0) return 1;
    } else if (ct_copy(mounts[*count].path, tokens[4]) != 0) return 1;
    if (snprintf(mounts[*count].filesystem, sizeof(mounts[*count].filesystem), "%s", tokens[dash + 1U]) >= (int)sizeof(mounts[*count].filesystem)) return 1;
    ++*count;
  }
  return 0;
}

static int ct_entry_add(struct ct_host_projection *selection, const char *source)
{
  char resolved[CT_HOST_PATH_MAX];
  const char *root = getenv("CT_HOST_PROJECTION_SOURCE_ROOT");
  const char *relative;
  char *target;
  size_t index;
  if (root == NULL || root[0] == '\0') root = "/";
  if (selection->entry_count == CT_HOST_PROJECTION_MAX_ENTRIES || !ct_host_projection_source_is_eligible(source, NULL) ||
      ct_root_relative(source, root, &relative) != 0) { (void)snprintf(selection->completeness, sizeof(selection->completeness), "partial"); return 0; }
  target = realpath(source, resolved);
  if (target == NULL || !ct_host_projection_source_is_eligible(target, NULL)) { (void)snprintf(selection->completeness, sizeof(selection->completeness), "partial"); return 0; }
  for (index = 0U; index < selection->entry_count; ++index) if (strcmp(selection->entries[index].source, source) == 0) return 0;
  if (ct_copy(selection->entries[selection->entry_count].source, source) != 0 || ct_copy(selection->entries[selection->entry_count].target, target) != 0 ||
       snprintf(selection->entries[selection->entry_count].destination, CT_HOST_PATH_MAX, "%s%s", "/host", strcmp(relative, "/") == 0 ? "" : relative) >= (int)CT_HOST_PATH_MAX) return 1;
  ++selection->entry_count;
  return 0;
}

static int ct_entry_compare(const void *left, const void *right)
{
  const char *left_path = ((const struct ct_host_projection_entry *)left)->source;
  const char *right_path = ((const struct ct_host_projection_entry *)right)->source;
  const size_t left_length = strlen(left_path), right_length = strlen(right_path);
  if (left_length != right_length && strncmp(left_path, right_path, left_length < right_length ? left_length : right_length) == 0) return left_length < right_length ? -1 : 1;
  return strcmp(left_path, right_path);
}

static int ct_build_direct(struct ct_host_projection *selection)
{
  struct ct_mountinfo *mounts;
  const char *root = getenv("CT_HOST_PROJECTION_SOURCE_ROOT");
  size_t count, index;
  if (root == NULL || root[0] == '\0') root = "/";
  selection->entry_count = 0U; (void)snprintf(selection->completeness, sizeof(selection->completeness), "complete");
  mounts = calloc(CT_HOST_CACHE_MAX_PAIRS, sizeof(*mounts));
  if (mounts == NULL || ct_mountinfo_read(mounts, &count) != 0 || ct_entry_add(selection, root) != 0) { free(mounts); return 1; }
  for (index = 0U; index < count; ++index) if (strcmp(mounts[index].path, root) != 0 && ct_host_projection_source_is_eligible(mounts[index].path, mounts[index].filesystem) != 0 && ct_entry_add(selection, mounts[index].path) != 0) { free(mounts); return 1; }
  qsort(selection->entries, selection->entry_count, sizeof(selection->entries[0]), ct_entry_compare);
  free(mounts);
  return 0;
}

static int ct_fallback_has_excluded_descendant(const struct ct_mountinfo mounts[], size_t count, const char *source)
{
  size_t index;
  const size_t length = strlen(source);
  for (index = 0U; index < count; ++index) {
    if (strncmp(mounts[index].path, source, length) == 0 && mounts[index].path[length] == '/' &&
        !ct_host_projection_source_is_eligible(mounts[index].path, mounts[index].filesystem)) return 1;
  }
  return 0;
}

static int ct_build_fallback_branch(struct ct_host_projection *selection, const struct ct_mountinfo mounts[], size_t count,
                                    const char *source, size_t depth, size_t *visited)
{
  DIR *directory;
  struct dirent *entry;
  if (!ct_host_projection_source_is_eligible(source, NULL)) return 0;
  if (!ct_fallback_has_excluded_descendant(mounts, count, source)) return ct_entry_add(selection, source);
  if (depth >= 64U || *visited >= CT_HOST_FALLBACK_MAX_VISITED) { (void)snprintf(selection->completeness, sizeof(selection->completeness), "partial"); return 0; }
  directory = ct_storage_timeout_opendir(source);
  if (directory == NULL) { (void)snprintf(selection->completeness, sizeof(selection->completeness), "partial"); return 0; }
  errno = 0;
  while ((entry = ct_storage_timeout_readdir(directory)) != NULL) {
    char child[CT_HOST_PATH_MAX];
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    if (*visited >= CT_HOST_FALLBACK_MAX_VISITED) { (void)snprintf(selection->completeness, sizeof(selection->completeness), "partial"); break; }
    ++*visited;
    if (snprintf(child, sizeof(child), "%s%s%s", source, strcmp(source, "/") == 0 ? "" : "/", entry->d_name) >= (int)sizeof(child) ||
        ct_build_fallback_branch(selection, mounts, count, child, depth + 1U, visited) != 0) { (void)ct_storage_timeout_closedir(directory); return 1; }
  }
  if (errno != 0 || ct_storage_timeout_closedir(directory) != 0) return 1;
  return 0;
}

static int ct_build_fallback(struct ct_host_projection *selection)
{
  struct ct_mountinfo *mounts;
  const char *root = getenv("CT_HOST_PROJECTION_SOURCE_ROOT");
  DIR *directory;
  struct dirent *entry;
  size_t count, visited = 0U;
  if (root == NULL || root[0] == '\0') root = "/";
  selection->entry_count = 0U; (void)snprintf(selection->completeness, sizeof(selection->completeness), "complete");
  mounts = calloc(CT_HOST_CACHE_MAX_PAIRS, sizeof(*mounts));
  if (mounts == NULL || ct_mountinfo_read(mounts, &count) != 0 || (directory = ct_storage_timeout_opendir(root)) == NULL) { free(mounts); return 1; }
  errno = 0;
  while ((entry = ct_storage_timeout_readdir(directory)) != NULL) {
    char child[CT_HOST_PATH_MAX];
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    if (visited >= CT_HOST_FALLBACK_MAX_VISITED) { (void)snprintf(selection->completeness, sizeof(selection->completeness), "partial"); break; }
    ++visited;
    if (snprintf(child, sizeof(child), "%s%s%s", root, strcmp(root, "/") == 0 ? "" : "/", entry->d_name) >= (int)sizeof(child)) { (void)ct_storage_timeout_closedir(directory); free(mounts); return 1; }
    if (ct_build_fallback_branch(selection, mounts, count, child, 0U, &visited) != 0) { (void)ct_storage_timeout_closedir(directory); free(mounts); return 1; }
  }
  if (errno != 0 || ct_storage_timeout_closedir(directory) != 0) { free(mounts); return 1; }
  qsort(selection->entries, selection->entry_count, sizeof(selection->entries[0]), ct_entry_compare);
  free(mounts);
  return 0;
}

static int ct_probe_nonce(char path[CT_HOST_PATH_MAX], char nonce[33])
{
  unsigned char bytes[16];
  size_t index;
  int descriptor;
  ssize_t received;
  static const char hexadecimal[] = "0123456789abcdef";

  if (snprintf(path, CT_HOST_PATH_MAX, "/tmp/container-tools-host-projection-%lu-XXXXXX", (unsigned long)geteuid()) >= (int)CT_HOST_PATH_MAX) return 1;
  descriptor = mkstemp(path);
  if (descriptor < 0 || fchmod(descriptor, 0600) != 0) { if (descriptor >= 0) (void)ct_storage_timeout_close(descriptor); return 1; }
  received = ct_storage_timeout_read(descriptor, bytes, sizeof(bytes));
  if (received != (ssize_t)sizeof(bytes)) {
    /* mkstemp did not create a random nonce source; use the kernel random device. */
    int random_descriptor = ct_storage_timeout_open("/dev/urandom", O_RDONLY | O_CLOEXEC, 0);
    if (random_descriptor < 0 || ct_storage_timeout_read(random_descriptor, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes) ||
        ct_storage_timeout_close(random_descriptor) != 0) { if (random_descriptor >= 0) (void)ct_storage_timeout_close(random_descriptor); (void)ct_storage_timeout_close(descriptor); return 1; }
  }
  for (index = 0U; index < sizeof(bytes); ++index) { nonce[index * 2U] = hexadecimal[bytes[index] >> 4U]; nonce[index * 2U + 1U] = hexadecimal[bytes[index] & 15U]; }
  nonce[32] = '\0';
  if (ct_storage_timeout_write(descriptor, nonce, strlen(nonce)) != (ssize_t)strlen(nonce) ||
      ct_storage_timeout_fsync(descriptor) != 0 || ct_storage_timeout_close(descriptor) != 0) { (void)ct_storage_timeout_unlink(path); return 1; }
  return 0;
}

static int ct_probe_sources_current(const struct ct_host_projection *selection)
{
  size_t index;
  for (index = 0U; index < selection->entry_count; ++index) {
    char resolved[CT_HOST_PATH_MAX];
    if (realpath(selection->entries[index].source, resolved) == NULL ||
        strcmp(resolved, selection->entries[index].target) != 0 ||
        !ct_host_projection_source_is_eligible(selection->entries[index].source, NULL) ||
        !ct_host_projection_source_is_eligible(resolved, NULL)) return 1;
  }
  return 0;
}

static int ct_probe_mount_descriptor(const char *backend, const struct ct_host_projection_entry *entry,
                                     char output[CT_HOST_PATH_MAX], const char **flag)
{
  if (entry == NULL || entry->source[0] != '/' || entry->destination[0] != '/') return 1;
  return ct_backend_outer_projection_mount(backend, entry->source,
                                           entry->destination, output,
                                           CT_HOST_PATH_MAX, flag);
}

/* The probe is deliberately a normal runtime operation: test doubles replace
 * only the executable, so the production planner, nonce check, and cleanup
 * sequence are always exercised. */
static int ct_probe_projection(const char *backend, const char *image, const struct ct_host_projection *selection)
{
  char nonce_path[CT_HOST_PATH_MAX], nonce[33], name[96];
  char mounts[CT_HOST_PROJECTION_MAX_ENTRIES + 2U][CT_HOST_PATH_MAX];
  char *arguments[CT_HOST_PROBE_MAX_ARGS] = {NULL};
  size_t argument_count = 0U, index;
  int created = 0, result, terminal = 0;

  if (image == NULL || image[0] == '\0' || ct_probe_sources_current(selection) != 0 || ct_probe_nonce(nonce_path, nonce) != 0) return 1;
  if (snprintf(name, sizeof(name), "ct-host-projection-%lu-%s", (unsigned long)geteuid(), nonce) >= (int)sizeof(name)) { (void)ct_storage_timeout_unlink(nonce_path); return 1; }
  if (strcmp(backend, "docker") == 0 || strcmp(backend, "podman") == 0) {
    arguments[argument_count++] = (char *)backend; arguments[argument_count++] = "create";
    arguments[argument_count++] = "--name"; arguments[argument_count++] = name;
    arguments[argument_count++] = "--label";
    if (snprintf(mounts[0], sizeof(mounts[0]), "container-tools.host-projection.probe=%s", name) >= (int)sizeof(mounts[0])) goto done;
    arguments[argument_count++] = mounts[0];
    arguments[argument_count++] = "--mount";
    if (snprintf(mounts[1], sizeof(mounts[1]), "type=bind,source=%s,target=/.ct-host-projection-locality,readonly", nonce_path) >= (int)sizeof(mounts[1])) goto done;
    arguments[argument_count++] = mounts[1];
    for (index = 0U; index < selection->entry_count; ++index) {
      const char *flag;
      if (argument_count + 2U >= CT_HOST_PROBE_MAX_ARGS || ct_probe_mount_descriptor(backend, &selection->entries[index], mounts[index + 2U], &flag) != 0) goto done;
      arguments[argument_count++] = (char *)flag; arguments[argument_count++] = mounts[index + 2U];
    }
    arguments[argument_count++] = (char *)image; arguments[argument_count++] = "/bin/sh"; arguments[argument_count++] = "-c";
    arguments[argument_count++] = "test -d /host && test \"$(cat /.ct-host-projection-locality)\" = \"$1\""; arguments[argument_count++] = "sh"; arguments[argument_count++] = nonce;
    arguments[argument_count] = NULL;
    created = ct_process_run_operation(arguments, "create") == 0;
    if (!created) {
      char owned[128];
      char *inspect[] = {(char *)backend, "container", "inspect", "--format", "{{ index .Config.Labels \"container-tools.host-projection.probe\" }}", name, NULL};
      if (ct_process_run_operation_capture(inspect, "cleanup", owned, sizeof(owned)) != 0) {
        terminal = 1;
        goto done;
      }
      owned[strcspn(owned, "\r\n")] = '\0';
      if (strcmp(owned, name) != 0) { terminal = 1; goto done; }
      { char *remove[] = {(char *)backend, "rm", "--force", name, NULL}; if (ct_process_run_operation(remove, "cleanup") != 0) terminal = 1; }
      goto done;
    }
    /* Docker and Podman create can report success with unusable output. The
     * name plus owned-label inspection, rather than create stdout, confirms
     * that cleanup and the following start address only our probe. */
    { char owned[128];
      char *inspect[] = {(char *)backend, "container", "inspect", "--format", "{{ index .Config.Labels \"container-tools.host-projection.probe\" }}", name, NULL};
      if (ct_process_run_operation_capture(inspect, "cleanup", owned, sizeof(owned)) != 0) {
        terminal = 1; created = 0; goto done;
      }
      owned[strcspn(owned, "\r\n")] = '\0';
      if (strcmp(owned, name) != 0) {
        terminal = 1; created = 0; goto done;
      } }
    { char *start[] = {(char *)backend, "start", "--attach", name, NULL}; result = ct_process_run_operation(start, "start"); }
  } else if (strcmp(backend, "singularity") == 0 || strcmp(backend, "apptainer") == 0) {
    arguments[argument_count++] = (char *)backend; arguments[argument_count++] = "exec";
    for (index = 0U; index < selection->entry_count; ++index) {
      const char *flag;
      if (argument_count + 2U >= CT_HOST_PROBE_MAX_ARGS || ct_probe_mount_descriptor(backend, &selection->entries[index], mounts[index], &flag) != 0) goto done;
      arguments[argument_count++] = (char *)flag; arguments[argument_count++] = mounts[index];
    }
    arguments[argument_count++] = (char *)image; arguments[argument_count++] = "/bin/sh"; arguments[argument_count++] = "-c"; arguments[argument_count++] = "test -d /host"; arguments[argument_count] = NULL;
    result = ct_process_run_operation(arguments, "probe");
  } else goto done;
  if (created) {
    char *remove[] = {(char *)backend, "rm", "--force", name, NULL};
    if (ct_process_run_operation(remove, "cleanup") != 0) terminal = 1;
    created = 0;
  }
  (void)ct_storage_timeout_unlink(nonce_path);
  return terminal ? 2 : (result == 0 ? 0 : 1);
done:
  if (created) {
    char *remove[] = {(char *)backend, "rm", "--force", name, NULL};
    if (ct_process_run_operation(remove, "cleanup") != 0) terminal = 1;
  }
  (void)ct_storage_timeout_unlink(nonce_path);
  return terminal ? 2 : 1;
}

static void ct_group_mode(const char *backend, char output[24])
{
  (void)snprintf(output, 24U, "%s", (strcmp(backend, "docker") == 0 || strcmp(backend, "podman") == 0) ? "primary-only" : "native-inherited");
}

int ct_host_projection_set_none(const char *backend,
                                struct ct_host_projection *selection)
{
  if (backend == NULL || selection == NULL ||
      (strcmp(backend, "docker") != 0 && strcmp(backend, "podman") != 0 &&
       strcmp(backend, "singularity") != 0 &&
       strcmp(backend, "apptainer") != 0)) return 1;
  memset(selection, 0, sizeof(*selection));
  (void)snprintf(selection->strategy, sizeof(selection->strategy), "none");
  (void)snprintf(selection->completeness, sizeof(selection->completeness),
                 "complete");
  ct_group_mode(backend, selection->group_mode);
  return 0;
}

static int ct_cache_root(char output[CT_HOST_PATH_MAX])
{
  const char *root = getenv("CT_HOST_PROJECTION_CACHE_ROOT");
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (root != NULL && root[0] != '\0') return ct_copy(output, root);
  if (runtime != NULL && runtime[0] == '/') return snprintf(output, CT_HOST_PATH_MAX, "%s/container-tools/host-projection-v1", runtime) >= (int)CT_HOST_PATH_MAX;
  return snprintf(output, CT_HOST_PATH_MAX, "/tmp/container-tools-%lu/host-projection-v1", (unsigned long)geteuid()) >= (int)CT_HOST_PATH_MAX;
}

static int ct_cache_key(const char *backend, const char *image, char output[65])
{
  struct ct_sha256 hash;
  unsigned char raw[32];
  char selector[512];
  const char *values[] = {"host-projection-v7", backend, image, getenv("CT_HOST_PROJECTION_SOURCE_ROOT"), selector};
  size_t index;
  if (ct_host_projection_selector(backend, selector) != 0) return 1;
  ct_sha256_init(&hash);
  for (index = 0U; index < sizeof(values) / sizeof(values[0]); ++index) { const char *value = values[index] == NULL ? "" : values[index]; ct_sha256_update(&hash, value, strlen(value) + 1U); }
  ct_sha256_final(&hash, raw); ct_sha256_hex(raw, output); return 0;
}

static int ct_cache_path(const char *key, char output[CT_HOST_PATH_MAX])
{
  char root[CT_HOST_PATH_MAX];
  return ct_cache_root(root) != 0 || ct_storage_ensure_private_directory(root) != 0 || snprintf(output, CT_HOST_PATH_MAX, "%s/%s", root, key) >= (int)CT_HOST_PATH_MAX;
}

static int ct_cache_lock(const char *key, int *descriptor)
{
  char root[CT_HOST_PATH_MAX], locks[CT_HOST_PATH_MAX], path[CT_HOST_PATH_MAX];
  struct stat path_status, descriptor_status;
  if (descriptor == NULL || ct_cache_root(root) != 0 || ct_storage_ensure_private_directory(root) != 0 ||
      snprintf(locks, sizeof(locks), "%s/.locks", root) >= (int)sizeof(locks) ||
      ct_storage_ensure_private_directory(locks) != 0 ||
      snprintf(path, sizeof(path), "%s/%s.lock", locks, key) >= (int)sizeof(path)) return 1;
  *descriptor = ct_storage_timeout_open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (*descriptor < 0 || ct_storage_timeout_lstat(path, &path_status) != 0 ||
      ct_storage_timeout_fstat(*descriptor, &descriptor_status) != 0 || !S_ISREG(path_status.st_mode) ||
      S_ISLNK(path_status.st_mode) || path_status.st_uid != geteuid() || (path_status.st_mode & 077U) != 0U ||
      descriptor_status.st_dev != path_status.st_dev || descriptor_status.st_ino != path_status.st_ino ||
      ct_storage_timeout_flock_lock(*descriptor) != 0) {
    (void)ct_storage_timeout_close(*descriptor);
    *descriptor = -1;
    return 1;
  }
  return 0;
}

static int ct_cache_file_identity(const struct stat *path_status, const struct stat *descriptor_status,
                                  size_t expected_size)
{
  return path_status != NULL && descriptor_status != NULL && S_ISREG(path_status->st_mode) &&
         !S_ISLNK(path_status->st_mode) && path_status->st_uid == geteuid() &&
         (path_status->st_mode & 077U) == 0U && path_status->st_size >= 0 &&
         (size_t)path_status->st_size == expected_size && S_ISREG(descriptor_status->st_mode) &&
         descriptor_status->st_uid == path_status->st_uid && descriptor_status->st_dev == path_status->st_dev &&
         descriptor_status->st_ino == path_status->st_ino && descriptor_status->st_size == path_status->st_size;
}

static int ct_cache_write_exact(int descriptor, const unsigned char *data, size_t length)
{
  size_t used = 0U;
  while (used < length) {
    const ssize_t written = ct_storage_timeout_write(descriptor, data + used, length - used);
    if (written <= 0) return 1;
    used += (size_t)written;
  }
  return 0;
}

/* Only a publisher holding this key's lock can recover its own interrupted
 * candidates. Other keys may still have an active publisher in this directory. */
static int ct_cache_recover_temporary_files(const char *key)
{
  char root[CT_HOST_PATH_MAX], prefix[80];
  DIR *directory;
  struct dirent *entry;
  size_t count = 0U;
  if (ct_cache_root(root) != 0 || snprintf(prefix, sizeof(prefix), "%s.tmp.", key) < 0 ||
      strlen(prefix) >= sizeof(prefix)) return 1;
  directory = ct_storage_timeout_opendir(root);
  if (directory == NULL) return 1;
  errno = 0;
  while ((entry = ct_storage_timeout_readdir(directory)) != NULL) {
    char stale[CT_HOST_PATH_MAX];
    struct stat status;
    if (++count > CT_HOST_CACHE_MAX_PAIRS) {
      (void)ct_storage_timeout_closedir(directory);
      return 1;
    }
    if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) continue;
    if (snprintf(stale, sizeof(stale), "%s/%s", root, entry->d_name) < 0 || strlen(stale) >= sizeof(stale) ||
        ct_storage_timeout_lstat(stale, &status) != 0 || !S_ISREG(status.st_mode) || S_ISLNK(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 077U) != 0U || ct_storage_timeout_unlink(stale) != 0) {
      (void)ct_storage_timeout_closedir(directory);
      return 1;
    }
  }
  return errno == 0 && ct_storage_timeout_closedir(directory) == 0 ? 0 : 1;
}

static int ct_cache_write(const char *key, const struct ct_host_projection *selection)
{
  char path[CT_HOST_PATH_MAX], temporary[CT_HOST_PATH_MAX] = "";
  unsigned char data[CT_HOST_CACHE_MAX_BYTES];
  size_t used = 0U, index;
  int descriptor = -1;
  struct stat status;
  #define CT_CACHE_FIELD(value) do { const size_t n = strlen(value) + 1U; if (n > sizeof(data) - used) return 1; memcpy(data + used, (value), n); used += n; } while (0)
  if (ct_cache_path(key, path) != 0 || ct_cache_recover_temporary_files(key) != 0) return 1;
  CT_CACHE_FIELD("ct-host-projection-selection-v1"); CT_CACHE_FIELD(key); CT_CACHE_FIELD(selection->strategy); CT_CACHE_FIELD(selection->completeness); CT_CACHE_FIELD(selection->group_mode); CT_CACHE_FIELD("proven");
  { char count[21]; (void)snprintf(count, sizeof(count), "%zu", strcmp(selection->strategy, "fallback") == 0 ? selection->entry_count : 0U); CT_CACHE_FIELD(count); }
  if (strcmp(selection->strategy, "fallback") == 0) for (index = 0U; index < selection->entry_count; ++index) { CT_CACHE_FIELD(selection->entries[index].source); CT_CACHE_FIELD(selection->entries[index].target); }
  for (index = 0U; index < 32U; ++index) {
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%lu", path, (long)getpid(), ++ct_cache_temporary_sequence) >= (int)sizeof(temporary)) return 1;
    descriptor = ct_storage_timeout_open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor >= 0 || errno != EEXIST) break;
  }
  if (descriptor < 0 || ct_cache_write_exact(descriptor, data, used) != 0 ||
      ct_storage_timeout_fsync(descriptor) != 0 || ct_storage_timeout_fstat(descriptor, &status) != 0 ||
      !S_ISREG(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 077U) != 0U ||
      status.st_size < 0 || (size_t)status.st_size != used || ct_storage_timeout_close(descriptor) != 0) {
    if (descriptor >= 0) (void)ct_storage_timeout_close(descriptor);
    (void)ct_storage_timeout_unlink(temporary);
    return 1;
  }
  descriptor = -1;
  /* rename(2) publishes either the complete previous record or the complete
   * refreshed record to unlocked warm readers; it never exposes the candidate. */
  if (ct_storage_timeout_rename(temporary, path) != 0) { (void)ct_storage_timeout_unlink(temporary); return 1; }
  return 0;
  #undef CT_CACHE_FIELD
}

static int ct_decimal(const char *value, size_t *number)
{
  size_t index; *number = 0U;
  if (value == NULL || value[0] == '\0' || (value[0] == '0' && value[1] != '\0')) return 1;
  for (index = 0U; value[index] != '\0'; ++index) { if (value[index] < '0' || value[index] > '9' || *number > (SIZE_MAX - (size_t)(value[index] - '0')) / 10U) return 1; *number = *number * 10U + (size_t)(value[index] - '0'); }
  return 0;
}

static int ct_cache_read(const char *key, struct ct_host_projection *selection)
{
  char path[CT_HOST_PATH_MAX], data[CT_HOST_CACHE_MAX_BYTES + 1U], *fields[7U + CT_HOST_CACHE_MAX_PAIRS * 2U];
  struct stat status, descriptor_status, final_status;
  int descriptor;
  ssize_t bytes = 0;
  size_t used = 0U, count = 0U, pairs, index, offset = 0U;
  if (ct_cache_path(key, path) != 0) return 2;
  if (ct_storage_timeout_lstat(path, &status) != 0) return errno == ENOENT ? 1 : 2;
  if (!S_ISREG(status.st_mode) || S_ISLNK(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 077U) != 0U || status.st_size <= 0 || (size_t)status.st_size > CT_HOST_CACHE_MAX_BYTES) return 2;
  descriptor = ct_storage_timeout_open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0);
  if (descriptor < 0 || ct_storage_timeout_fstat(descriptor, &descriptor_status) != 0 ||
      !ct_cache_file_identity(&status, &descriptor_status, (size_t)status.st_size)) { if (descriptor >= 0) (void)ct_storage_timeout_close(descriptor); return 2; }
  while (used < (size_t)status.st_size && (bytes = ct_storage_timeout_read(descriptor, data + used, (size_t)status.st_size - used)) > 0) used += (size_t)bytes;
  if (ct_storage_timeout_close(descriptor) != 0 || bytes < 0 || used != (size_t)status.st_size || data[used - 1U] != '\0' ||
      ct_storage_timeout_lstat(path, &final_status) != 0 ||
      !ct_cache_file_identity(&status, &final_status, (size_t)status.st_size)) return 2;
  while (offset < used && count < sizeof(fields) / sizeof(fields[0])) {
    fields[count++] = data + offset;
    while (offset < used && data[offset] != '\0') ++offset;
    if (offset == used) return 2;
    ++offset;
  }
  if (offset != used) return 2;
  if (count < 7U || strcmp(fields[0], "ct-host-projection-selection-v1") != 0 || strcmp(fields[1], key) != 0 || ct_decimal(fields[6], &pairs) != 0 || pairs > CT_HOST_CACHE_MAX_PAIRS ||
      (strcmp(fields[2], "direct") != 0 && strcmp(fields[2], "fallback") != 0 && strcmp(fields[2], "none") != 0) ||
      (strcmp(fields[3], "complete") != 0 && strcmp(fields[3], "partial") != 0) ||
      (strcmp(fields[4], "primary-only") != 0 && strcmp(fields[4], "numeric-supplementary") != 0 && strcmp(fields[4], "keep-groups") != 0 && strcmp(fields[4], "native-inherited") != 0) ||
       (strcmp(fields[2], "fallback") == 0 ? count != 7U + pairs * 2U : (pairs != 0U || count != 7U))) return 2;
  (void)snprintf(selection->strategy, sizeof(selection->strategy), "%s", fields[2]); (void)snprintf(selection->completeness, sizeof(selection->completeness), "%s", fields[3]); (void)snprintf(selection->group_mode, sizeof(selection->group_mode), "%s", fields[4]); selection->entry_count = 0U;
  if (strcmp(fields[2], "fallback") == 0) for (index = 0U; index < pairs; ++index) { if (fields[7U + index * 2U][0] != '/' || fields[8U + index * 2U][0] != '/' || ct_entry_add(selection, fields[7U + index * 2U]) != 0) return 2; if (strcmp(selection->entries[selection->entry_count - 1U].target, fields[8U + index * 2U]) != 0) (void)snprintf(selection->completeness, sizeof(selection->completeness), "partial"); }
  return 0;
}

int ct_host_projection_prepare(const char *backend, const char *image, const char *mode, int refresh, struct ct_host_projection *selection)
{
  char key[65];
  int cache_hit = 0;
  int cache_status;
  int lock_descriptor = -1;
  int terminal_failure = 0;
  if (backend == NULL || image == NULL || mode == NULL || selection == NULL || (strcmp(mode, "auto") != 0 && strcmp(mode, "required") != 0 && strcmp(mode, "disabled") != 0)) return 1;
  memset(selection, 0, sizeof(*selection)); ct_group_mode(backend, selection->group_mode);
  if (strcmp(mode, "disabled") == 0) return ct_host_projection_set_none(backend, selection);
  if (!ct_host_projection_endpoint_is_local(backend)) return 1;
  if (ct_cache_key(backend, image, key) != 0) return 1;
  if (ct_cache_lock(key, &lock_descriptor) != 0) return 1;
  cache_status = refresh ? 1 : ct_cache_read(key, selection);
  /* Cache bytes are advisory. An unrecognized record becomes a cold miss and
   * remains untouched until a complete current selection is ready to publish. */
  if (cache_status == 2) {
    memset(selection, 0, sizeof(*selection));
    ct_group_mode(backend, selection->group_mode);
    cache_status = 1;
  }
  if (cache_status == 0) cache_hit = 1;
  if (cache_hit && strcmp(selection->strategy, "direct") == 0) { if (ct_build_direct(selection) != 0) goto failed; }
  if (cache_hit && ct_probe_sources_current(selection) != 0) goto failed;
  if (!cache_hit) {
    if (strcmp(backend, "docker") == 0 || strcmp(backend, "podman") == 0) {
      const int probe = ct_build_direct(selection) == 0 ? ct_probe_projection(backend, image, selection) : 1;
      if (probe == 0) (void)snprintf(selection->strategy, sizeof(selection->strategy), "direct");
      else if (probe == 2) { terminal_failure = 1; goto failed; }
    }
    if (selection->strategy[0] == '\0') {
      const int probe = ct_build_fallback(selection) == 0 ? ct_probe_projection(backend, image, selection) : 1;
      if (probe == 0) (void)snprintf(selection->strategy, sizeof(selection->strategy), "fallback");
      else if (probe == 2) { terminal_failure = 1; goto failed; }
    }
    if (selection->strategy[0] == '\0' &&
        ct_host_projection_set_none(backend, selection) != 0) goto failed;
    if (ct_probe_sources_current(selection) != 0 || ct_cache_write(key, selection) != 0) goto failed;
  }
  (void)ct_storage_timeout_flock_unlock(lock_descriptor);
  (void)ct_storage_timeout_close(lock_descriptor);
  return 0;
failed:
  (void)ct_storage_timeout_flock_unlock(lock_descriptor);
  (void)ct_storage_timeout_close(lock_descriptor);
  return terminal_failure ? 2 : 1;
}
