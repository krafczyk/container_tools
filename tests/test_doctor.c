/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "doctor.h"
#include "yyjson.h"

#include <stdio.h>
#include <string.h>

static int object_keys(yyjson_val *object, const char *const *expected,
                       size_t expected_count)
{
  yyjson_obj_iter iterator;
  yyjson_val *key;
  size_t index = 0U;
  if (!yyjson_is_obj(object) || yyjson_obj_size(object) != expected_count ||
      !yyjson_obj_iter_init(object, &iterator)) return 1;
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    if (index >= expected_count || !yyjson_equals_str(key, expected[index++])) return 1;
  }
  return index == expected_count ? 0 : 1;
}

static int nullable_string(yyjson_val *value, const char *expected)
{
  return expected == NULL ? !yyjson_is_null(value)
                          : !yyjson_is_str(value) ||
                                strcmp(yyjson_get_str(value), expected) != 0;
}

static int backend(yyjson_val *entry, const char *name, int installed,
                   int eligible, const char *operational, const char *reason)
{
  static const char *const keys[] = {
      "name", "installed", "eligible", "operational", "reason_code", "detail"};
  return object_keys(entry, keys, sizeof(keys) / sizeof(keys[0])) != 0 ||
                 !yyjson_equals_str(yyjson_obj_get(entry, "name"), name) ||
                 !yyjson_is_bool(yyjson_obj_get(entry, "installed")) ||
                 yyjson_get_bool(yyjson_obj_get(entry, "installed")) != (installed != 0) ||
                 !yyjson_is_bool(yyjson_obj_get(entry, "eligible")) ||
                 yyjson_get_bool(yyjson_obj_get(entry, "eligible")) != (eligible != 0) ||
                 !yyjson_equals_str(yyjson_obj_get(entry, "operational"), operational) ||
                 !yyjson_equals_str(yyjson_obj_get(entry, "reason_code"), reason) ||
                 !yyjson_is_str(yyjson_obj_get(entry, "detail"));
}

static int validate_report(const char *output, size_t length,
                           const struct ct_doctor_mount_plan *mount_plan,
                           const struct ct_nested_backend_report reports[3])
{
  static const char *const root_keys[] = {
      "schema", "schema_version", "profile", "architecture", "requested_semantics",
      "mount_plan", "backends"};
  static const char *const mount_keys[] = {
      "configured", "status", "digest", "strategy", "completeness", "detail"};
  static const char *const names[] = {"bubblewrap", "proot", "rewrite"};
  yyjson_doc *document;
  yyjson_val *root;
  yyjson_val *mount;
  yyjson_val *backends;
  size_t index;
  if (length == 0U || output[length - 1U] != '\n') return 1;
  document = yyjson_read(output, length - 1U, YYJSON_READ_NOFLAG);
  if (document == NULL) return 1;
  root = yyjson_doc_get_root(document);
  mount = yyjson_obj_get(root, "mount_plan");
  backends = yyjson_obj_get(root, "backends");
  if (object_keys(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0])) != 0 ||
      !yyjson_equals_str(yyjson_obj_get(root, "schema"),
                         "container-tools.host-doctor/v1") ||
      !yyjson_is_uint(yyjson_obj_get(root, "schema_version")) ||
      yyjson_get_uint(yyjson_obj_get(root, "schema_version")) != CT_DOCTOR_SCHEMA_VERSION ||
      !yyjson_is_str(yyjson_obj_get(root, "profile")) ||
      !yyjson_is_str(yyjson_obj_get(root, "architecture")) ||
      !yyjson_is_str(yyjson_obj_get(root, "requested_semantics")) ||
      object_keys(mount, mount_keys, sizeof(mount_keys) / sizeof(mount_keys[0])) != 0 ||
      !yyjson_is_bool(yyjson_obj_get(mount, "configured")) ||
      yyjson_get_bool(yyjson_obj_get(mount, "configured")) !=
          (mount_plan->configured != 0) ||
      !yyjson_equals_str(yyjson_obj_get(mount, "status"), mount_plan->status) ||
      nullable_string(yyjson_obj_get(mount, "digest"), mount_plan->digest) != 0 ||
      nullable_string(yyjson_obj_get(mount, "strategy"), mount_plan->strategy) != 0 ||
      nullable_string(yyjson_obj_get(mount, "completeness"),
                      mount_plan->completeness) != 0 ||
      !yyjson_equals_str(yyjson_obj_get(mount, "detail"), mount_plan->detail) ||
      !yyjson_is_arr(backends) || yyjson_arr_size(backends) != 3U) {
    yyjson_doc_free(document);
    return 1;
  }
  for (index = 0U; index < 3U; ++index) {
    if (backend(yyjson_arr_get(backends, index), names[index], reports[index].installed,
                reports[index].eligible,
                reports[index].operational == CT_NESTED_OPERATIONAL_YES
                    ? "yes"
                    : reports[index].operational == CT_NESTED_OPERATIONAL_NO
                          ? "no"
                          : "not-probed",
                reports[index].reason_code) != 0) {
      yyjson_doc_free(document);
      return 1;
    }
  }
  yyjson_doc_free(document);
  return 0;
}

static int render_and_validate(const struct ct_host_profile *profile,
                               const struct ct_doctor_mount_plan *mount_plan,
                               const struct ct_nested_backend_report reports[3])
{
  FILE *stream = tmpfile();
  char output[8192];
  size_t length;
  if (stream == NULL || ct_doctor_json(stream, profile, mount_plan, reports) != 0 ||
      fseek(stream, 0L, SEEK_SET) != 0) return 1;
  length = fread(output, 1U, sizeof(output), stream);
  if (fclose(stream) != 0 || length == sizeof(output)) return 1;
  return validate_report(output, length, mount_plan, reports);
}

int main(void)
{
  struct ct_host_profile profile;
  struct ct_doctor_mount_plan mount_plan;
  struct ct_nested_backend_report reports[3];
  FILE *stream;
  char byte;
  memset(&profile, 0, sizeof(profile));
  strcpy(profile.name, "host\"profile");
  strcpy(profile.semantics, "full-root");
  memset(reports, 0, sizeof(reports));
  reports[0] = (struct ct_nested_backend_report){CT_NESTED_BACKEND_BUBBLEWRAP, 1, 1,
                                                   CT_NESTED_OPERATIONAL_YES, "ready", ""};
  reports[1] = (struct ct_nested_backend_report){CT_NESTED_BACKEND_PROOT, 1, 1,
                                                   CT_NESTED_OPERATIONAL_NOT_PROBED,
                                                   "earlier-backend-ready", ""};
  reports[2] = (struct ct_nested_backend_report){CT_NESTED_BACKEND_REWRITE, 1, 0,
                                                   CT_NESTED_OPERATIONAL_NOT_PROBED,
                                                   "degradation-not-authorized", ""};
  mount_plan = (struct ct_doctor_mount_plan){1, "ready",
      "0123456789012345678901234567890123456789012345678901234567890123",
      "direct", "complete", ""};
  if (render_and_validate(&profile, &mount_plan, reports) != 0) return 1;

  reports[0].installed = 0; reports[0].eligible = 0;
  reports[0].operational = CT_NESTED_OPERATIONAL_NO; reports[0].reason_code = "not-installed";
  reports[1].installed = 1; reports[1].eligible = 0;
  reports[1].operational = CT_NESTED_OPERATIONAL_NOT_PROBED;
  reports[1].reason_code = "incompatible-profile";
  reports[2].installed = 1; reports[2].eligible = 1;
  reports[2].operational = CT_NESTED_OPERATIONAL_YES; reports[2].reason_code = "ready";
  mount_plan = (struct ct_doctor_mount_plan){0, "disabled", NULL, NULL, NULL,
                                              "mount plan disabled by profile"};
  if (render_and_validate(&profile, &mount_plan, reports) != 0) return 2;

  stream = tmpfile();
  mount_plan.status = NULL;
  if (stream == NULL || ct_doctor_json(stream, &profile, &mount_plan, reports) == 0 ||
      fseek(stream, 0L, SEEK_SET) != 0 || fread(&byte, 1U, 1U, stream) != 0 ||
      fclose(stream) != 0) return 3;
  return 0;
}
