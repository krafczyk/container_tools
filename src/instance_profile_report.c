/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "instance_profile_report.h"

#include "yyjson.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

static char *ct_ipr_escape(const char *value)
{
  static const char hex[] = "0123456789abcdef";
  const unsigned char *source = (const unsigned char *)value;
  size_t input, output = 0U, length;
  char *escaped;
  if (value == NULL) return NULL;
  length = strlen(value);
  if (length > (SIZE_MAX - 1U) / 4U || (escaped = malloc(length * 4U + 1U)) == NULL) return NULL;
  for (input = 0U; input < length; ++input) {
    unsigned char byte = source[input];
    if (byte == '\\' || byte == '"') { escaped[output++] = '\\'; escaped[output++] = (char)byte; }
    else if (byte >= 0x20U && byte <= 0x7eU) escaped[output++] = (char)byte;
    else { escaped[output++] = '\\'; escaped[output++] = 'x'; escaped[output++] = hex[byte >> 4U]; escaped[output++] = hex[byte & 15U]; }
  }
  escaped[output] = '\0'; return escaped;
}
static int ct_ipr_begin(struct sigaction *previous)
{
  struct sigaction ignored;
  if (sigaction(SIGPIPE, NULL, previous) != 0) return 1;
  memset(&ignored, 0, sizeof(ignored)); ignored.sa_handler = SIG_IGN;
  return sigemptyset(&ignored.sa_mask) != 0 || sigaction(SIGPIPE, &ignored, NULL) != 0;
}
static int ct_ipr_end(FILE *stream, const struct sigaction *previous)
{
  int failed = stream == NULL || fflush(stream) != 0 || ferror(stream) != 0;
  return sigaction(SIGPIPE, previous, NULL) != 0 || failed;
}
static int ct_ipr_add_string(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key, const char *value, int path)
{
  char *escaped = path != 0 ? ct_ipr_escape(value) : NULL;
  int result = (path != 0 ? escaped == NULL || !yyjson_mut_obj_add_strcpy(doc, object, key, escaped) : !yyjson_mut_obj_add_strcpy(doc, object, key, value));
  free(escaped); return result;
}
static char *ct_ipr_escape_identity(const char *value)
{
  return strcmp(value, "absent") == 0 ? strdup(value) : ct_ipr_escape(value);
}
int ct_instance_profile_report_write_json(FILE *stream, const struct ct_instance_profile_manifest *m)
{
  struct sigaction previous; yyjson_mut_doc *doc; yyjson_mut_val *root, *groups, *mounts; char *text = NULL; size_t length, index; int failed = 1;
  if (stream == NULL || m == NULL || ct_ipr_begin(&previous) != 0) return 1;
  doc = yyjson_mut_doc_new(NULL); root = doc == NULL ? NULL : yyjson_mut_obj(doc); groups = doc == NULL ? NULL : yyjson_mut_arr(doc); mounts = doc == NULL ? NULL : yyjson_mut_arr(doc);
  if (doc == NULL || root == NULL || groups == NULL || mounts == NULL) goto done;
  yyjson_mut_doc_set_root(doc, root);
  if (!yyjson_mut_obj_add_strcpy(doc, root, "schema", "container-tools.instance-profile-inspect/v1") || !yyjson_mut_obj_add_uint(doc, root, "schema_version", 1U) || !yyjson_mut_obj_add_strcpy(doc, root, "path_encoding", "backslash-escaped-bytes") ||
      ct_ipr_add_string(doc, root, "record_digest", m->record_digest, 0) || ct_ipr_add_string(doc, root, "profile_digest", m->profile_digest, 0) || ct_ipr_add_string(doc, root, "instance_name", m->instance_name, 0) || ct_ipr_add_string(doc, root, "backend", m->backend, 0) || ct_ipr_add_string(doc, root, "profile_grammar", m->profile_grammar, 0) || ct_ipr_add_string(doc, root, "runtime_argument_digest", m->runtime_argument_digest, 0) || ct_ipr_add_string(doc, root, "uid", m->uid, 0) || ct_ipr_add_string(doc, root, "gid", m->gid, 0) || ct_ipr_add_string(doc, root, "hostname", m->hostname, 0) || ct_ipr_add_string(doc, root, "home", m->home, 1) || ct_ipr_add_string(doc, root, "instance_root", m->instance_root, 1) || ct_ipr_add_string(doc, root, "image_path", m->image_path, 1) || ct_ipr_add_string(doc, root, "image_identity", m->image_identity, 0) || ct_ipr_add_string(doc, root, "bootstrap_path", m->bootstrap_path, 1) || ct_ipr_add_string(doc, root, "bootstrap_identity", m->bootstrap_identity, strcmp(m->bootstrap_identity, "absent") != 0) || ct_ipr_add_string(doc, root, "projection_grammar", m->projection_grammar, 0) || ct_ipr_add_string(doc, root, "projection_digest", m->projection_digest, 0) || ct_ipr_add_string(doc, root, "group_mode", m->group_mode, 0) || ct_ipr_add_string(doc, root, "mount_plan_digest", m->mount_plan_digest, 0)) goto done;
  for (index = 0U; index < m->group_count; ++index) if (!yyjson_mut_arr_add_strcpy(doc, groups, m->groups[index])) goto done;
  if (!yyjson_mut_obj_add_val(doc, root, "supplementary_groups", groups)) goto done;
  for (index = 0U; index < m->mount_count; ++index) { const struct ct_instance_profile_manifest_mount *x = &m->mounts[index]; yyjson_mut_val *item = yyjson_mut_obj(doc); if (item == NULL || ct_ipr_add_string(doc, item, "flag", x->flag, 0) || ct_ipr_add_string(doc, item, "descriptor", x->descriptor, 1) || ct_ipr_add_string(doc, item, "role", x->role, 0) || ct_ipr_add_string(doc, item, "access", x->access, 0) || ct_ipr_add_string(doc, item, "recursion", x->recursion, 0) || !yyjson_mut_obj_add_bool(doc, item, "generated", x->generated != 0) || !yyjson_mut_obj_add_bool(doc, item, "semantic", x->semantic != 0) || ct_ipr_add_string(doc, item, "resolved_source", x->resolved_source, 1) || ct_ipr_add_string(doc, item, "source_identity", x->source_identity, strcmp(x->source_identity, "absent") != 0) || !yyjson_mut_arr_add_val(mounts, item)) goto done; }
  if (!yyjson_mut_obj_add_val(doc, root, "mounts", mounts) || (text = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, &length)) == NULL || fwrite(text, 1U, length, stream) != length || fputc('\n', stream) == EOF) goto done;
  failed = 0;
done: free(text); yyjson_mut_doc_free(doc); return ct_ipr_end(stream, &previous) != 0 || failed;
}
int ct_instance_profile_report_write_human(FILE *stream, const struct ct_instance_profile_manifest *m)
{
  struct sigaction previous; size_t index; int failed;
  char *home, *root, *image, *bootstrap, *bootstrap_identity;
  if (stream == NULL || m == NULL || ct_ipr_begin(&previous) != 0) return 1;
  home = ct_ipr_escape(m->home); root = ct_ipr_escape(m->instance_root); image = ct_ipr_escape(m->image_path); bootstrap = ct_ipr_escape(m->bootstrap_path); bootstrap_identity = ct_ipr_escape_identity(m->bootstrap_identity);
  failed = home == NULL || root == NULL || image == NULL || bootstrap == NULL || bootstrap_identity == NULL || fprintf(stream, "record digest: %s\nprofile digest: %s\ninstance name: %s\nbackend: %s\nprofile grammar: %s\nruntime argument digest: %s\nuid: %s\ngid: %s\nhostname: %s\nHOME: %s\ninstance root: %s\nimage path: %s\nimage identity: %s\nbootstrap path: %s\nbootstrap identity: %s\nprojection grammar: %s\nprojection digest: %s\ngroup mode: %s\nmount plan digest: %s\n", m->record_digest, m->profile_digest, m->instance_name, m->backend, m->profile_grammar, m->runtime_argument_digest, m->uid, m->gid, m->hostname, home, root, image, m->image_identity, bootstrap, bootstrap_identity, m->projection_grammar, m->projection_digest, m->group_mode, m->mount_plan_digest) < 0;
  free(home); free(root); free(image); free(bootstrap); free(bootstrap_identity);
  for (index = 0U; !failed && index < m->group_count; ++index) failed = fprintf(stream, "group %zu: %s\n", index, m->groups[index]) < 0;
  for (index = 0U; !failed && index < m->mount_count; ++index) { const struct ct_instance_profile_manifest_mount *x = &m->mounts[index]; char *descriptor = ct_ipr_escape(x->descriptor); char *source = ct_ipr_escape(x->resolved_source); char *identity = ct_ipr_escape_identity(x->source_identity); failed = descriptor == NULL || source == NULL || identity == NULL || fprintf(stream, "mount %zu: flag=%s descriptor=%s role=%s access=%s recursion=%s generated=%s semantic=%s resolved source=%s source identity=%s\n", index, x->flag, descriptor, x->role, x->access, x->recursion, x->generated ? "true" : "false", x->semantic ? "true" : "false", source, identity) < 0; free(descriptor); free(source); free(identity); }
  return ct_ipr_end(stream, &previous) != 0 || failed;
}
