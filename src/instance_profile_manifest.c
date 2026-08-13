/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "instance_profile_manifest.h"

#include "profile.h"
#include "sha256.h"
#include "storage.h"
#include "storage_timeout.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CT_IPM_FIXED_FIELDS 22U
#define CT_IPM_MOUNT_FIELDS 9U
#define CT_IPM_MAX_FIELDS (CT_IPM_FIXED_FIELDS + CT_INSTANCE_PROFILE_MANIFEST_MAX_GROUPS + CT_INSTANCE_PROFILE_MANIFEST_MAX_MOUNTS * CT_IPM_MOUNT_FIELDS)

static int ct_ipm_digest(const char *value) { return value != NULL && strlen(value) == 64U && strspn(value, "0123456789abcdef") == 64U; }
static int ct_ipm_decimal(const char *value, size_t *result)
{
  size_t number = 0U, index;
  if (value == NULL || value[0] == '\0' || (value[0] == '0' && value[1] != '\0')) return 1;
  for (index = 0U; value[index] != '\0'; ++index) {
    if (value[index] < '0' || value[index] > '9' || number > (SIZE_MAX - (size_t)(value[index] - '0')) / 10U) return 1;
    number = number * 10U + (size_t)(value[index] - '0');
  }
  *result = number;
  return 0;
}
static int ct_ipm_one_of(const char *value, const char *const values[], size_t count)
{
  size_t index;
  if (value == NULL) return 0;
  for (index = 0U; index < count; ++index) if (strcmp(value, values[index]) == 0) return 1;
  return 0;
}
static int ct_ipm_scalar(const char *value) { return value != NULL && value[0] != '\0' && strchr(value, '\n') == NULL; }
static int ct_ipm_path(const char *value) { return ct_ipm_scalar(value) && value[0] == '/'; }
static int ct_ipm_mount_valid(const struct ct_instance_profile_manifest_mount *mount)
{
  static const char *const flags[] = {"--bind", "--mount"};
  static const char *const roles[] = {"generated-host-root", "detected-automatic", "explicit", "bootstrap-internal", "persistent-automatic-cwd", "state-mask", "manifest-internal"};
  static const char *const access[] = {"inherit", "read-only"};
  static const char *const recursion[] = {"non-recursive", "runtime-default"};
  return mount != NULL && ct_ipm_one_of(mount->flag, flags, 2U) && ct_ipm_scalar(mount->descriptor) &&
          ct_ipm_one_of(mount->role, roles, 7U) && ct_ipm_one_of(mount->access, access, 2U) &&
         ct_ipm_one_of(mount->recursion, recursion, 2U) && (mount->generated == 0 || mount->generated == 1) &&
         (mount->semantic == 0 || mount->semantic == 1) && ct_ipm_path(mount->resolved_source) &&
         (strcmp(mount->source_identity, "absent") == 0 || ct_ipm_scalar(mount->source_identity));
}

static int ct_ipm_size_add(size_t *total, const char *value)
{
  const size_t length = strlen(value) + 1U;
  if (length > CT_INSTANCE_PROFILE_MANIFEST_MAX_BYTES - *total) return 1;
  *total += length;
  return 0;
}

static int ct_ipm_serialized_size(
    const struct ct_instance_profile_manifest *m, size_t *size)
{
  char groups[21], mounts[21];
  size_t total = 65U, index;
  const char *const fixed[] = {
      "ct-instance-profile-v1", m->profile_digest, m->instance_name,
      m->backend, m->profile_grammar, m->runtime_argument_digest, m->uid,
      m->gid, m->hostname, m->home, m->instance_root, m->image_path,
      m->image_identity, m->bootstrap_path, m->bootstrap_identity,
      m->projection_grammar, m->projection_digest, m->group_mode};
  if (snprintf(groups, sizeof(groups), "%zu", m->group_count) < 0 ||
      snprintf(mounts, sizeof(mounts), "%zu", m->mount_count) < 0)
    return 1;
  for (index = 0U; index < sizeof(fixed) / sizeof(fixed[0]); ++index) {
    if (ct_ipm_size_add(&total, fixed[index]) != 0) return 1;
  }
  if (ct_ipm_size_add(&total, groups) != 0) return 1;
  for (index = 0U; index < m->group_count; ++index) {
    if (ct_ipm_size_add(&total, m->groups[index]) != 0) return 1;
  }
  if (ct_ipm_size_add(&total, m->mount_plan_digest) != 0 ||
      ct_ipm_size_add(&total, mounts) != 0) return 1;
  for (index = 0U; index < m->mount_count; ++index) {
    const struct ct_instance_profile_manifest_mount *x = &m->mounts[index];
    const char *const values[] = {x->flag, x->descriptor, x->role, x->access,
                                  x->recursion, x->resolved_source,
                                  x->source_identity};
    size_t value_index;
    for (value_index = 0U;
         value_index < sizeof(values) / sizeof(values[0]); ++value_index) {
      if (ct_ipm_size_add(&total, values[value_index]) != 0) return 1;
    }
    if (4U > CT_INSTANCE_PROFILE_MANIFEST_MAX_BYTES - total) return 1;
    total += 4U;
  }
  *size = total;
  return 0;
}

int ct_instance_profile_manifest_profile_digest(
    const struct ct_instance_profile_manifest *m, char digest[65])
{
  const char **fields;
  const char *bootstrap_identity;
  size_t count = 0U, index;
  int result;

  if (m == NULL || digest == NULL || m->backend == NULL ||
      m->runtime_argument_digest == NULL || m->uid == NULL || m->gid == NULL ||
      m->hostname == NULL || m->home == NULL || m->instance_root == NULL ||
      m->image_path == NULL || m->image_identity == NULL ||
      m->bootstrap_identity == NULL || m->profile_grammar == NULL ||
      m->projection_grammar == NULL || m->projection_digest == NULL ||
      m->group_mode == NULL ||
      (m->group_count != 0U && m->groups == NULL) ||
      (m->mount_count != 0U && m->mounts == NULL) ||
      m->group_count > CT_INSTANCE_PROFILE_MANIFEST_MAX_GROUPS ||
      m->mount_count > CT_INSTANCE_PROFILE_MANIFEST_MAX_MOUNTS) return 1;
  fields = calloc(18U + m->group_count + m->mount_count * 3U,
                  sizeof(*fields));
  if (fields == NULL) return 1;
#define PROFILE_FIELD(value) fields[count++] = (value)
  bootstrap_identity = strcmp(m->bootstrap_identity, "absent") == 0
                           ? ""
                           : m->bootstrap_identity;
  PROFILE_FIELD(m->backend);
  PROFILE_FIELD(m->runtime_argument_digest);
  PROFILE_FIELD(m->uid);
  PROFILE_FIELD(m->hostname);
  PROFILE_FIELD(m->home);
  PROFILE_FIELD(m->instance_root);
  PROFILE_FIELD(m->image_path);
  PROFILE_FIELD(m->image_identity);
  PROFILE_FIELD(bootstrap_identity);
  PROFILE_FIELD(m->profile_grammar);
  PROFILE_FIELD(m->projection_grammar);
  PROFILE_FIELD(m->projection_digest);
  PROFILE_FIELD(m->uid);
  PROFILE_FIELD(m->gid);
  PROFILE_FIELD(m->group_mode);
  for (index = 0U; index < m->group_count; ++index)
    PROFILE_FIELD(m->groups[index]);
  for (index = 0U; index < m->mount_count; ++index) {
    if (m->mounts[index].role == NULL ||
        m->mounts[index].resolved_source == NULL ||
        m->mounts[index].flag == NULL ||
        m->mounts[index].descriptor == NULL ||
        m->mounts[index].source_identity == NULL) {
      free(fields);
      return 1;
    }
    if (strcmp(m->mounts[index].role, "state-mask") == 0 &&
        strcmp(m->mounts[index].resolved_source, m->instance_root) == 0)
      continue;
    PROFILE_FIELD(m->mounts[index].flag);
    PROFILE_FIELD(m->mounts[index].descriptor);
  }
  for (index = 0U; index < m->mount_count; ++index) {
    if (strcmp(m->mounts[index].role, "state-mask") == 0 &&
        strcmp(m->mounts[index].resolved_source, m->instance_root) == 0)
      continue;
    if (strcmp(m->mounts[index].source_identity, "absent") != 0)
      PROFILE_FIELD(m->mounts[index].source_identity);
  }
#undef PROFILE_FIELD
  result = ct_profile_digest_fields(fields, count, digest);
  free(fields);
  return result;
}

int ct_instance_profile_manifest_normalize_root(const char *value, char output[4096])
{
  char copy[4096], *part, *state = NULL, *parts[2048];
  size_t count = 0U, used = 1U, index;
  if (value == NULL || value[0] != '/' || strchr(value, ':') != NULL || strchr(value, ',') != NULL || strchr(value, '\n') != NULL ||
      snprintf(copy, sizeof(copy), "%s", value) >= (int)sizeof(copy)) return 1;
  part = strtok_r(copy, "/", &state);
  while (part != NULL) {
    if (strcmp(part, ".") == 0) { part = strtok_r(NULL, "/", &state); continue; }
    if (strcmp(part, "..") == 0) { if (count != 0U) --count; }
    else if (count == sizeof(parts) / sizeof(parts[0])) return 1;
    else parts[count++] = part;
    part = strtok_r(NULL, "/", &state);
  }
  output[0] = '/'; output[1] = '\0';
  for (index = 0U; index < count; ++index) {
    const size_t length = strlen(parts[index]);
    if (used + (used > 1U ? 1U : 0U) + length >= 4096U) return 1;
    if (used > 1U) output[used++] = '/';
    memcpy(output + used, parts[index], length); used += length; output[used] = '\0';
  }
  return 0;
}

int ct_instance_profile_manifest_validate(const struct ct_instance_profile_manifest *m)
{
  static const char *const backends[] = {"singularity", "apptainer"};
  static const char *const group_modes[] = {"numeric-supplementary", "keep-groups", "primary-only", "native-inherited", "none"};
  size_t index, bytes;
  char expected[40], normalized[4096], computed_profile[65];
  if (m == NULL || m->profile_grammar == NULL ||
      m->runtime_argument_digest == NULL || m->bootstrap_path == NULL ||
      m->bootstrap_identity == NULL || m->projection_grammar == NULL ||
      !ct_ipm_digest(m->profile_digest) || !ct_ipm_one_of(m->backend, backends, 2U) ||
      strcmp(m->profile_grammar, "ct-instance-profile-v4") != 0 ||
      !(strcmp(m->runtime_argument_digest, "absent") == 0 || ct_ipm_digest(m->runtime_argument_digest)) ||
      !ct_ipm_scalar(m->uid) || !ct_ipm_scalar(m->gid) || !ct_ipm_scalar(m->hostname) || !ct_ipm_path(m->home) ||
      ct_instance_profile_manifest_normalize_root(m->instance_root, normalized) != 0 || strcmp(normalized, m->instance_root) != 0 ||
      !ct_ipm_path(m->image_path) || !ct_ipm_scalar(m->image_identity) ||
      !((strcmp(m->bootstrap_path, "absent") == 0 && strcmp(m->bootstrap_identity, "absent") == 0) ||
        (ct_ipm_path(m->bootstrap_path) && ct_ipm_scalar(m->bootstrap_identity))) ||
      strcmp(m->projection_grammar, "ct-host-projection-profile-v1") != 0 || !ct_ipm_digest(m->projection_digest) ||
      !ct_ipm_one_of(m->group_mode, group_modes, 5U) || !ct_ipm_digest(m->mount_plan_digest) ||
      m->group_count > CT_INSTANCE_PROFILE_MANIFEST_MAX_GROUPS || m->mount_count > CT_INSTANCE_PROFILE_MANIFEST_MAX_MOUNTS ||
      (m->group_count != 0U && m->groups == NULL) || (m->mount_count != 0U && m->mounts == NULL) ||
      snprintf(expected, sizeof(expected), "mkchad-%.32s", m->profile_digest) >= (int)sizeof(expected) ||
      m->instance_name == NULL || strcmp(m->instance_name, expected) != 0) return 1;
  for (index = 0U; index < m->group_count; ++index) if (!ct_ipm_scalar(m->groups[index])) return 1;
  for (index = 0U; index < m->mount_count; ++index) if (!ct_ipm_mount_valid(&m->mounts[index])) return 1;
  if (ct_instance_profile_manifest_profile_digest(m, computed_profile) != 0 ||
      strcmp(computed_profile, m->profile_digest) != 0) return 1;
  return ct_ipm_serialized_size(m, &bytes);
}
static int ct_ipm_add(unsigned char *out, size_t capacity, size_t *used, const char *value, struct ct_sha256 *hash)
{
  size_t length = strlen(value) + 1U;
  if (*used > capacity || length > capacity - *used) return 1;
  memcpy(out + *used, value, length); if (hash != NULL) ct_sha256_update(hash, value, length); *used += length; return 0;
}
static int ct_ipm_write_fields(unsigned char *out, size_t capacity, size_t *used, const struct ct_instance_profile_manifest *m, struct ct_sha256 *hash)
{
  char groups[21], mounts[21], generated[2], semantic[2]; size_t index;
  if (snprintf(groups, sizeof(groups), "%zu", m->group_count) < 0 || snprintf(mounts, sizeof(mounts), "%zu", m->mount_count) < 0) return 1;
#define ADD(value) if (ct_ipm_add(out, capacity, used, value, hash) != 0) return 1
  ADD(m->profile_digest); ADD(m->instance_name); ADD(m->backend); ADD(m->profile_grammar); ADD(m->runtime_argument_digest); ADD(m->uid); ADD(m->gid); ADD(m->hostname); ADD(m->home); ADD(m->instance_root); ADD(m->image_path); ADD(m->image_identity); ADD(m->bootstrap_path); ADD(m->bootstrap_identity); ADD(m->projection_grammar); ADD(m->projection_digest); ADD(m->group_mode); ADD(groups);
  for (index = 0U; index < m->group_count; ++index) ADD(m->groups[index]);
  ADD(m->mount_plan_digest); ADD(mounts);
  for (index = 0U; index < m->mount_count; ++index) { const struct ct_instance_profile_manifest_mount *x = &m->mounts[index]; (void)snprintf(generated, sizeof(generated), "%d", x->generated); (void)snprintf(semantic, sizeof(semantic), "%d", x->semantic); ADD(x->flag); ADD(x->descriptor); ADD(x->role); ADD(x->access); ADD(x->recursion); ADD(generated); ADD(semantic); ADD(x->resolved_source); ADD(x->source_identity); }
#undef ADD
  return 0;
}
int ct_instance_profile_manifest_serialize(const struct ct_instance_profile_manifest *m, unsigned char **bytes, size_t *length)
{
  unsigned char *out, raw[32]; struct ct_sha256 hash;
  size_t capacity, used = 0U;
  if (bytes == NULL || length == NULL || ct_instance_profile_manifest_validate(m) != 0) return 1;
  if (ct_ipm_serialized_size(m, &capacity) != 0) return 1;
  out = malloc(capacity); if (out == NULL) return 1;
  ct_sha256_init(&hash);
  if (ct_ipm_add(out, capacity, &used, "ct-instance-profile-v1", &hash) != 0) goto bad;
  used += 65U;
  if (ct_ipm_write_fields(out, capacity, &used, m, &hash) != 0) goto bad;
  ct_sha256_final(&hash, raw); ct_sha256_hex(raw, (char *)(out + strlen("ct-instance-profile-v1") + 1U));
  out[strlen("ct-instance-profile-v1") + 65U] = '\0'; *bytes = out; *length = used; return 0;
bad: free(out); return 1;
}
static int ct_ipm_split(const unsigned char *bytes, size_t length, char **copy, char *fields[CT_IPM_MAX_FIELDS], size_t *count)
{
  size_t offset = 0U, used = 0U;
  if (bytes == NULL || length == 0U || length > CT_INSTANCE_PROFILE_MANIFEST_MAX_BYTES || bytes[length - 1U] != '\0' || (*copy = malloc(length)) == NULL) return 1;
  while (offset < length) { size_t start = offset; if (used == CT_IPM_MAX_FIELDS) goto bad; while (offset < length && bytes[offset] != '\0') ++offset; if (offset == length) goto bad; memcpy(*copy + start, bytes + start, offset - start); (*copy)[offset] = '\0'; fields[used++] = *copy + start; ++offset; }
  *count = used; return 0;
bad: free(*copy); return 1;
}
static enum ct_instance_profile_manifest_read_status ct_ipm_decode(const unsigned char *bytes, size_t length, struct ct_instance_profile_manifest *m)
{
  char *copy = NULL, *fields[CT_IPM_MAX_FIELDS], computed[65]; size_t count, groups, mounts, index, field = 0U; struct ct_sha256 hash; unsigned char raw[32];
  memset(m, 0, sizeof(*m));
  if (ct_ipm_split(bytes, length, &copy, fields, &count) != 0) return CT_INSTANCE_PROFILE_MANIFEST_READ_MALFORMED;
  if (strcmp(fields[0], "ct-instance-profile-v1") != 0) {
    const enum ct_instance_profile_manifest_read_status status =
        strncmp(fields[0], "ct-instance-profile-v",
                strlen("ct-instance-profile-v")) == 0
            ? CT_INSTANCE_PROFILE_MANIFEST_READ_FUTURE
            : CT_INSTANCE_PROFILE_MANIFEST_READ_MALFORMED;
    free(copy);
    return status;
  }
  if (count < CT_IPM_FIXED_FIELDS || !ct_ipm_digest(fields[1]) || !ct_ipm_digest(fields[2]) || ct_ipm_decimal(fields[19], &groups) != 0 || groups > CT_INSTANCE_PROFILE_MANIFEST_MAX_GROUPS || count < 22U + groups) { free(copy); return CT_INSTANCE_PROFILE_MANIFEST_READ_MALFORMED; }
  field = 21U + groups; if (ct_ipm_decimal(fields[field], &mounts) != 0 || mounts > CT_INSTANCE_PROFILE_MANIFEST_MAX_MOUNTS || count != field + 1U + mounts * CT_IPM_MOUNT_FIELDS) { free(copy); return CT_INSTANCE_PROFILE_MANIFEST_READ_MALFORMED; }
  m->groups = calloc(groups, sizeof(*m->groups)); m->mounts = calloc(mounts, sizeof(*m->mounts));
  if ((groups != 0U && m->groups == NULL) || (mounts != 0U && m->mounts == NULL)) { free(copy); ct_instance_profile_manifest_destroy(m); return CT_INSTANCE_PROFILE_MANIFEST_READ_IO; }
  (void)snprintf(m->record_digest, sizeof(m->record_digest), "%s", fields[1]); (void)snprintf(m->profile_digest, sizeof(m->profile_digest), "%s", fields[2]);
  m->instance_name = strdup(fields[3]); m->backend = strdup(fields[4]); m->profile_grammar = strdup(fields[5]); m->runtime_argument_digest = strdup(fields[6]); m->uid = strdup(fields[7]); m->gid = strdup(fields[8]); m->hostname = strdup(fields[9]); m->home = strdup(fields[10]); m->instance_root = strdup(fields[11]); m->image_path = strdup(fields[12]); m->image_identity = strdup(fields[13]); m->bootstrap_path = strdup(fields[14]); m->bootstrap_identity = strdup(fields[15]); m->projection_grammar = strdup(fields[16]); m->projection_digest = strdup(fields[17]); m->group_mode = strdup(fields[18]); m->group_count = groups;
  for (index = 0U; index < groups; ++index) m->groups[index] = strdup(fields[20U + index]);
  m->mount_plan_digest = strdup(fields[20U + groups]); m->mount_count = mounts; field = 22U + groups;
  for (index = 0U; index < mounts; ++index) { struct ct_instance_profile_manifest_mount *x = &m->mounts[index]; size_t generated, semantic; x->flag = strdup(fields[field++]); x->descriptor = strdup(fields[field++]); x->role = strdup(fields[field++]); x->access = strdup(fields[field++]); x->recursion = strdup(fields[field++]); if (ct_ipm_decimal(fields[field++], &generated) != 0 || ct_ipm_decimal(fields[field++], &semantic) != 0) { free(copy); ct_instance_profile_manifest_destroy(m); return CT_INSTANCE_PROFILE_MANIFEST_READ_MALFORMED; } x->generated = (int)generated; x->semantic = (int)semantic; x->resolved_source = strdup(fields[field++]); x->source_identity = strdup(fields[field++]); }
  if (m->instance_name == NULL || m->backend == NULL || m->profile_grammar == NULL || m->runtime_argument_digest == NULL || m->uid == NULL || m->gid == NULL || m->hostname == NULL || m->home == NULL || m->instance_root == NULL || m->image_path == NULL || m->image_identity == NULL || m->bootstrap_path == NULL || m->bootstrap_identity == NULL || m->projection_grammar == NULL || m->projection_digest == NULL || m->group_mode == NULL || m->mount_plan_digest == NULL) { free(copy); ct_instance_profile_manifest_destroy(m); return CT_INSTANCE_PROFILE_MANIFEST_READ_IO; }
  for (index = 0U; index < groups; ++index) if (m->groups[index] == NULL) { free(copy); ct_instance_profile_manifest_destroy(m); return CT_INSTANCE_PROFILE_MANIFEST_READ_IO; }
  for (index = 0U; index < mounts; ++index) { const struct ct_instance_profile_manifest_mount *x = &m->mounts[index]; if (x->flag == NULL || x->descriptor == NULL || x->role == NULL || x->access == NULL || x->recursion == NULL || x->resolved_source == NULL || x->source_identity == NULL) { free(copy); ct_instance_profile_manifest_destroy(m); return CT_INSTANCE_PROFILE_MANIFEST_READ_IO; } }
  if (ct_instance_profile_manifest_validate(m) != 0) { free(copy); ct_instance_profile_manifest_destroy(m); return CT_INSTANCE_PROFILE_MANIFEST_READ_SEMANTIC_INVALID; }
  ct_sha256_init(&hash); for (index = 0U; index < count; ++index) if (index != 1U) ct_sha256_update(&hash, fields[index], strlen(fields[index]) + 1U); ct_sha256_final(&hash, raw); ct_sha256_hex(raw, computed);
  free(copy); if (strcmp(computed, m->record_digest) != 0) { ct_instance_profile_manifest_destroy(m); return CT_INSTANCE_PROFILE_MANIFEST_READ_DIGEST_MISMATCH; } return CT_INSTANCE_PROFILE_MANIFEST_READ_OK;
}
int ct_instance_profile_manifest_parse(const unsigned char *bytes, size_t length) { struct ct_instance_profile_manifest m; enum ct_instance_profile_manifest_read_status status = ct_ipm_decode(bytes, length, &m); ct_instance_profile_manifest_destroy(&m); return status == CT_INSTANCE_PROFILE_MANIFEST_READ_OK ? 0 : 1; }
enum ct_instance_profile_manifest_read_status ct_instance_profile_manifest_parse_read(
    const unsigned char *bytes, size_t length,
    struct ct_instance_profile_manifest *manifest)
{
  if (manifest == NULL) return CT_INSTANCE_PROFILE_MANIFEST_READ_MALFORMED;
  return ct_ipm_decode(bytes, length, manifest);
}
void ct_instance_profile_manifest_destroy(struct ct_instance_profile_manifest *m)
{
  size_t index; if (m == NULL) return;
#define FREE(field) free((void *)m->field)
  FREE(instance_name); FREE(backend); FREE(profile_grammar); FREE(runtime_argument_digest); FREE(uid); FREE(gid); FREE(hostname); FREE(home); FREE(instance_root); FREE(image_path); FREE(image_identity); FREE(bootstrap_path); FREE(bootstrap_identity); FREE(projection_grammar); FREE(projection_digest); FREE(group_mode); FREE(mount_plan_digest);
  for (index = 0U; index < m->group_count; ++index) {
    free((void *)m->groups[index]);
  }
  free((void *)m->groups);
  for (index = 0U; index < m->mount_count; ++index) {
    struct ct_instance_profile_manifest_mount *x = &m->mounts[index];
    free((void *)x->flag);
    free((void *)x->descriptor);
    free((void *)x->role);
    free((void *)x->access);
    free((void *)x->recursion);
    free((void *)x->resolved_source);
    free((void *)x->source_identity);
  }
  free(m->mounts);
#undef FREE
  memset(m, 0, sizeof(*m));
}
enum ct_instance_profile_manifest_read_status ct_instance_profile_manifest_read(const char *path, struct ct_instance_profile_manifest *m)
{
  struct stat before, after, final; unsigned char *bytes; int fd; size_t used = 0U;
  if (path == NULL || m == NULL) return CT_INSTANCE_PROFILE_MANIFEST_READ_MALFORMED;
  memset(m, 0, sizeof(*m));
  fd = ct_storage_timeout_open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0); if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? CT_INSTANCE_PROFILE_MANIFEST_READ_ABSENT : CT_INSTANCE_PROFILE_MANIFEST_READ_IO;
  if (ct_storage_timeout_fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 1 || (uintmax_t)before.st_size > CT_INSTANCE_PROFILE_MANIFEST_MAX_BYTES || (bytes = malloc((size_t)before.st_size)) == NULL) { (void)ct_storage_timeout_close(fd); return CT_INSTANCE_PROFILE_MANIFEST_READ_IO; }
  while (used < (size_t)before.st_size) { ssize_t got = ct_storage_timeout_read(fd, bytes + used, (size_t)before.st_size - used); if (got <= 0) { free(bytes); (void)ct_storage_timeout_close(fd); return CT_INSTANCE_PROFILE_MANIFEST_READ_IO; } used += (size_t)got; }
  if (ct_storage_timeout_fstat(fd, &after) != 0 || ct_storage_timeout_close(fd) != 0 ||
      before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
      ct_storage_timeout_stat(path, &final) != 0 || !S_ISREG(final.st_mode) || final.st_dev != before.st_dev || final.st_ino != before.st_ino || final.st_size != before.st_size || final.st_mtim.tv_sec != before.st_mtim.tv_sec || final.st_mtim.tv_nsec != before.st_mtim.tv_nsec || final.st_ctim.tv_sec != before.st_ctim.tv_sec || final.st_ctim.tv_nsec != before.st_ctim.tv_nsec) { free(bytes); return CT_INSTANCE_PROFILE_MANIFEST_READ_CHANGED; }
  { enum ct_instance_profile_manifest_read_status result = ct_ipm_decode(bytes, used, m); free(bytes); return result; }
}

static int ct_ipm_private_directory(const char *path)
{
  struct stat status;
  return ct_storage_timeout_lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0777U) != 0700U;
}
static int ct_ipm_private_file(const char *path)
{
  struct stat status;
  return ct_storage_timeout_lstat(path, &status) != 0 || !S_ISREG(status.st_mode) || S_ISLNK(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0777U) != 0600U;
}
static int ct_ipm_cache_paths(const char *root, const char *digest, char profiles[4096], char path[4096])
{
  return root == NULL || !ct_ipm_digest(digest) || snprintf(profiles, 4096U, "%s/profiles", root) >= 4096 || snprintf(path, 4096U, "%s/%s.manifest", profiles, digest) >= 4096;
}
static int ct_ipm_write_exact(int fd, const unsigned char *bytes, size_t length)
{
  size_t used = 0U;
  while (used < length) { ssize_t wrote = ct_storage_timeout_write(fd, bytes + used, length - used); if (wrote <= 0) return 1; used += (size_t)wrote; }
  return 0;
}
enum ct_instance_profile_manifest_read_status ct_instance_profile_manifest_read_private(
    const char *root, const char *digest, struct ct_instance_profile_manifest *manifest)
{
  char profiles[4096], path[4096];
  enum ct_instance_profile_manifest_read_status status;
  if (manifest == NULL) return CT_INSTANCE_PROFILE_MANIFEST_READ_IO;
  memset(manifest, 0, sizeof(*manifest));
  if (ct_ipm_cache_paths(root, digest, profiles, path) != 0 ||
      ct_ipm_private_directory(root) != 0 || ct_ipm_private_directory(profiles) != 0 ||
      ct_ipm_private_file(path) != 0) return CT_INSTANCE_PROFILE_MANIFEST_READ_IO;
  status = ct_instance_profile_manifest_read(path, manifest);
  if (status != CT_INSTANCE_PROFILE_MANIFEST_READ_OK || strcmp(manifest->profile_digest, digest) != 0) { ct_instance_profile_manifest_destroy(manifest); return status == CT_INSTANCE_PROFILE_MANIFEST_READ_OK ? CT_INSTANCE_PROFILE_MANIFEST_READ_DIGEST_MISMATCH : status; }
  return CT_INSTANCE_PROFILE_MANIFEST_READ_OK;
}
int ct_instance_profile_manifest_publish(
    const char *root, const struct ct_instance_profile_manifest *manifest,
    char path[4096])
{
  char profiles[4096] = "", target[4096] = "", temporary[4096] = "";
  unsigned char *bytes = NULL;
  size_t length = 0U;
  int fd = -1, result = 1;
  struct ct_instance_profile_manifest expected, existing;
  memset(&expected, 0, sizeof(expected));
  memset(&existing, 0, sizeof(existing));
  if (path == NULL || manifest == NULL || ct_instance_profile_manifest_serialize(manifest, &bytes, &length) != 0 || ct_instance_profile_manifest_parse_read(bytes, length, &expected) != CT_INSTANCE_PROFILE_MANIFEST_READ_OK || ct_ipm_cache_paths(root, manifest->profile_digest, profiles, target) != 0 || ct_storage_ensure_private_directory(root) != 0 || ct_storage_ensure_private_directory(profiles) != 0 || ct_ipm_private_directory(root) != 0 || ct_ipm_private_directory(profiles) != 0 || snprintf(temporary, sizeof(temporary), "%s/.tmp.%s.XXXXXX", profiles, manifest->profile_digest) >= (int)sizeof(temporary)) goto done;
  if (ct_instance_profile_manifest_read_private(
          root, manifest->profile_digest,
          &existing) == CT_INSTANCE_PROFILE_MANIFEST_READ_OK) {
    if (strcmp(existing.record_digest, expected.record_digest) != 0 ||
        snprintf(path, 4096U, "%s", target) >= 4096) goto done;
    result = 0;
    goto done;
  }
  fd = mkstemp(temporary);
  if (fd < 0 || ct_storage_timeout_chmod(temporary, 0600) != 0 || ct_ipm_write_exact(fd, bytes, length) != 0 || ct_storage_timeout_fsync(fd) != 0 || ct_storage_timeout_close(fd) != 0) { if (fd >= 0) (void)ct_storage_timeout_close(fd); fd = -1; goto done; }
  fd = -1;
  if (ct_storage_timeout_link(temporary, target) != 0) {
    if (errno != EEXIST || ct_instance_profile_manifest_read_private(root, manifest->profile_digest, &existing) != CT_INSTANCE_PROFILE_MANIFEST_READ_OK || strcmp(existing.record_digest, expected.record_digest) != 0) { ct_instance_profile_manifest_destroy(&existing); goto done; }
    ct_instance_profile_manifest_destroy(&existing);
  }
  if (ct_instance_profile_manifest_read_private(root, manifest->profile_digest, &existing) != CT_INSTANCE_PROFILE_MANIFEST_READ_OK || strcmp(existing.record_digest, expected.record_digest) != 0) { ct_instance_profile_manifest_destroy(&existing); goto done; }
  ct_instance_profile_manifest_destroy(&existing);
  if (snprintf(path, 4096U, "%s", target) >= 4096) goto done;
  result = 0;
done:
  if (fd >= 0) (void)ct_storage_timeout_close(fd);
  (void)ct_storage_timeout_unlink(temporary);
  ct_instance_profile_manifest_destroy(&expected);
  ct_instance_profile_manifest_destroy(&existing);
  free(bytes);
  return result;
}
