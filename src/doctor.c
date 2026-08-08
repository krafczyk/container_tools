/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "doctor.h"

#include "package_identity.h"
#include "yyjson.h"

#include <stdlib.h>

static const char *ct_doctor_operational(enum ct_nested_operational operational)
{
  if (operational == CT_NESTED_OPERATIONAL_YES) return "yes";
  if (operational == CT_NESTED_OPERATIONAL_NO) return "no";
  return "not-probed";
}

int ct_doctor_json(
    FILE *stream, const struct ct_host_profile *profile,
    const struct ct_doctor_mount_plan *mount_plan,
    const struct ct_nested_backend_report reports[CT_NESTED_BACKEND_COUNT])
{
  yyjson_mut_doc *document;
  yyjson_mut_val *root;
  yyjson_mut_val *mount;
  yyjson_mut_val *backends;
  char *output = NULL;
  size_t output_length = 0U;
  size_t index;
  int result = 1;
  if (stream == NULL || profile == NULL || mount_plan == NULL ||
      mount_plan->status == NULL || mount_plan->detail == NULL || reports == NULL) {
    return 1;
  }
  document = yyjson_mut_doc_new(NULL);
  root = document == NULL ? NULL : yyjson_mut_obj(document);
  mount = document == NULL ? NULL : yyjson_mut_obj(document);
  backends = document == NULL ? NULL : yyjson_mut_arr(document);
  if (document == NULL || root == NULL || mount == NULL || backends == NULL) goto done;
  yyjson_mut_doc_set_root(document, root);
  if (!yyjson_mut_obj_add_strcpy(document, root, "schema",
                                 "container-tools.host-doctor/v1") ||
      !yyjson_mut_obj_add_int(document, root, "schema_version",
                              CT_DOCTOR_SCHEMA_VERSION) ||
      !yyjson_mut_obj_add_strcpy(document, root, "profile", profile->name) ||
      !yyjson_mut_obj_add_strcpy(document, root, "architecture", CT_ARCHITECTURE) ||
      !yyjson_mut_obj_add_strcpy(document, root, "requested_semantics",
                                 profile->semantics) ||
      !yyjson_mut_obj_add_bool(document, mount, "configured",
                               mount_plan->configured != 0) ||
      !yyjson_mut_obj_add_strcpy(document, mount, "status", mount_plan->status)) {
    goto done;
  }
  if ((mount_plan->digest == NULL
           ? !yyjson_mut_obj_add_null(document, mount, "digest")
           : !yyjson_mut_obj_add_strcpy(document, mount, "digest",
                                        mount_plan->digest)) ||
      (mount_plan->strategy == NULL
           ? !yyjson_mut_obj_add_null(document, mount, "strategy")
           : !yyjson_mut_obj_add_strcpy(document, mount, "strategy",
                                        mount_plan->strategy)) ||
      (mount_plan->completeness == NULL
           ? !yyjson_mut_obj_add_null(document, mount, "completeness")
           : !yyjson_mut_obj_add_strcpy(document, mount, "completeness",
                                        mount_plan->completeness)) ||
      !yyjson_mut_obj_add_strcpy(document, mount, "detail", mount_plan->detail) ||
      !yyjson_mut_obj_add_val(document, root, "mount_plan", mount)) {
    goto done;
  }
  for (index = 0U; index < CT_NESTED_BACKEND_COUNT; ++index) {
    yyjson_mut_val *entry = yyjson_mut_obj(document);
    const char *name = ct_backend_nested_name(reports[index].backend);
    if (entry == NULL || name == NULL || reports[index].reason_code == NULL ||
        !yyjson_mut_obj_add_strcpy(document, entry, "name", name) ||
        !yyjson_mut_obj_add_bool(document, entry, "installed",
                                 reports[index].installed != 0) ||
        !yyjson_mut_obj_add_bool(document, entry, "eligible",
                                 reports[index].eligible != 0) ||
        !yyjson_mut_obj_add_strcpy(
            document, entry, "operational",
            ct_doctor_operational(reports[index].operational)) ||
        !yyjson_mut_obj_add_strcpy(document, entry, "reason_code",
                                   reports[index].reason_code) ||
        !yyjson_mut_obj_add_strcpy(document, entry, "detail",
                                   reports[index].detail) ||
        !yyjson_mut_arr_add_val(backends, entry)) {
      goto done;
    }
  }
  if (!yyjson_mut_obj_add_val(document, root, "backends", backends)) goto done;
  output = yyjson_mut_write(document, YYJSON_WRITE_NOFLAG, &output_length);
  if (output == NULL || output_length == 0U ||
      fwrite(output, 1U, output_length, stream) != output_length ||
      fputc('\n', stream) == EOF) {
    goto done;
  }
  result = 0;
done:
  free(output);
  yyjson_mut_doc_free(document);
  return result;
}
