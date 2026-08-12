/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "mount_plan_report.h"

#include "yyjson.h"

#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#define CT_MOUNT_PLAN_PATH_ENCODING "backslash-escaped-bytes"

void ct_mount_plan_report_init(struct ct_mount_plan_report *report)
{
  if (report != NULL) memset(report, 0, sizeof(*report));
}

static void ct_mount_plan_report_entry_destroy(
    struct ct_mount_plan_report_entry *entry)
{
  if (entry == NULL) return;
  free(entry->role);
  free(entry->caller_path);
  free(entry->target_path);
  free(entry->access);
  free(entry->recursion);
  memset(entry, 0, sizeof(*entry));
}

struct ct_mount_plan_report_load {
  struct ct_mount_plan_report *report;
  size_t copied_entry_count;
  int copy_failed;
};

static void ct_mount_plan_report_destroy_entries(
    struct ct_mount_plan_report *report, size_t entry_count)
{
  size_t index;
  if (report == NULL) return;
  for (index = 0U; index < entry_count; ++index) {
    ct_mount_plan_report_entry_destroy(&report->entries[index]);
  }
  free(report->entries);
  memset(report, 0, sizeof(*report));
}

void ct_mount_plan_report_destroy(struct ct_mount_plan_report *report)
{
  ct_mount_plan_report_destroy_entries(
      report, report == NULL ? 0U : report->metadata.entry_count);
}

static int ct_mount_plan_report_entry_copy(
    const struct ct_mount_plan_entry *entry, void *context)
{
  struct ct_mount_plan_report_load *load = context;
  struct ct_mount_plan_report *report;
  size_t index;
  struct ct_mount_plan_report_entry *copy;

  if (entry == NULL || load == NULL || load->report == NULL ||
      load->copied_entry_count >= CT_MOUNT_PLAN_MAX_ENTRIES) return 1;
  report = load->report;
  if (report->entries == NULL) {
#ifdef CT_MOUNT_PLAN_REPORT_TEST_SEAM
    if (getenv("CT_MOUNT_PLAN_REPORT_TEST_FAIL_ALLOCATIONS") != NULL) {
      load->copy_failed = 1;
      return 1;
    }
#endif
    report->entries = calloc(report->metadata.entry_count, sizeof(*report->entries));
    if (report->entries == NULL) { load->copy_failed = 1; return 1; }
  }
  index = load->copied_entry_count;
  copy = &report->entries[index];
  copy->role = strdup(entry->role);
  copy->caller_path = strdup(entry->caller_path);
  copy->target_path = strdup(entry->target_path);
  copy->access = strdup(entry->access);
  copy->recursion = strdup(entry->recursion);
  if (copy->role == NULL || copy->caller_path == NULL || copy->target_path == NULL ||
      copy->access == NULL || copy->recursion == NULL) {
    ct_mount_plan_report_entry_destroy(copy);
    load->copy_failed = 1;
    return 1;
  }
  ++load->copied_entry_count;
  return 0;
}

enum ct_mount_plan_read_status ct_mount_plan_report_read(
    const char *path, struct ct_mount_plan_report *report)
{
  struct ct_mount_plan_report loaded;
  struct ct_mount_plan_report_load load;
  enum ct_mount_plan_read_status status;

  if (report == NULL) return CT_MOUNT_PLAN_READ_MALFORMED;
  ct_mount_plan_report_init(&loaded);
  load = (struct ct_mount_plan_report_load){&loaded, 0U, 0};
  status = ct_mount_plan_read_status(path, &loaded.metadata,
                                     ct_mount_plan_report_entry_copy, &load);
  if (load.copy_failed != 0) status = CT_MOUNT_PLAN_READ_IO;
  if (status != CT_MOUNT_PLAN_READ_OK) {
    ct_mount_plan_report_destroy_entries(&loaded, load.copied_entry_count);
    return status;
  }
  if (load.copied_entry_count == loaded.metadata.entry_count) {
    ct_mount_plan_report_destroy(report);
    *report = loaded;
    return CT_MOUNT_PLAN_READ_OK;
  }
  ct_mount_plan_report_destroy_entries(&loaded, load.copied_entry_count);
  return CT_MOUNT_PLAN_READ_IO;
}

static char *ct_mount_plan_report_escape_path(const char *value)
{
  static const char hexadecimal[] = "0123456789abcdef";
  const unsigned char *source = (const unsigned char *)value;
  size_t input, output = 0U, length;
  char *escaped;
  if (value == NULL) return NULL;
  length = strlen(value);
  if (length > (SIZE_MAX - 1U) / 4U) return NULL;
  escaped = malloc(length * 4U + 1U);
  if (escaped == NULL) return NULL;
  for (input = 0U; input < length; ++input) {
    const unsigned char byte = source[input];
    if (byte == '\\' || byte == '"') {
      escaped[output++] = '\\';
      escaped[output++] = (char)byte;
    } else if (byte >= 0x20U && byte <= 0x7eU) {
      escaped[output++] = (char)byte;
    } else {
      escaped[output++] = '\\';
      escaped[output++] = 'x';
      escaped[output++] = hexadecimal[byte >> 4U];
      escaped[output++] = hexadecimal[byte & 15U];
    }
  }
  escaped[output] = '\0';
  return escaped;
}

struct ct_mount_plan_output {
  struct sigaction previous;
  int active;
};

static int ct_mount_plan_output_begin(struct ct_mount_plan_output *output)
{
  struct sigaction ignored;
  if (output == NULL || sigaction(SIGPIPE, NULL, &output->previous) != 0) return 1;
  memset(&ignored, 0, sizeof(ignored));
  ignored.sa_handler = SIG_IGN;
  if (sigemptyset(&ignored.sa_mask) != 0 ||
      sigaction(SIGPIPE, &ignored, NULL) != 0) return 1;
  output->active = 1;
  return 0;
}

static int ct_mount_plan_output_end(FILE *stream,
                                    struct ct_mount_plan_output *output)
{
  int result = stream == NULL || fflush(stream) != 0 || ferror(stream) != 0;
  if (output != NULL && output->active != 0) {
    if (sigaction(SIGPIPE, &output->previous, NULL) != 0) result = 1;
    output->active = 0;
  }
  return result;
}

static int ct_mount_plan_report_write_metadata_human(
    FILE *stream, const struct ct_mount_plan_report *report, const char *prefix)
{
  return fprintf(stream,
                 "%sgrammar: %s\n%sdigest: %s\n%sbackend: %s\n%sstrategy: %s\n"
                 "%scompleteness: %s\n%sgroup mode: %s\n%sentry count: %zu\n",
                 prefix, report->metadata.grammar, prefix, report->metadata.digest,
                 prefix, report->metadata.backend, prefix, report->metadata.strategy,
                 prefix, report->metadata.completeness, prefix,
                 report->metadata.group_mode, prefix, report->metadata.entry_count) < 0;
}

static int ct_mount_plan_report_write_entries_human(
    FILE *stream, const struct ct_mount_plan_report *report, const char *prefix)
{
  size_t index;
  for (index = 0U; index < report->metadata.entry_count; ++index) {
    const struct ct_mount_plan_report_entry *entry = &report->entries[index];
    char *caller = ct_mount_plan_report_escape_path(entry->caller_path);
    char *target = ct_mount_plan_report_escape_path(entry->target_path);
    int result;
    if (caller == NULL || target == NULL) { free(caller); free(target); return 1; }
    result = fprintf(stream,
                 "%sentry %zu:\n%s  role: %s\n%s  caller path: %s\n"
                 "%s  target path: %s\n%s  access: %s\n%s  recursion: %s\n",
                 prefix, index, prefix, entry->role, prefix, caller,
                 prefix, target, prefix, entry->access, prefix,
                 entry->recursion) < 0;
    free(caller);
    free(target);
    if (result) return 1;
  }
  return 0;
}

int ct_mount_plan_report_write_human(FILE *stream,
                                     const struct ct_mount_plan_report *report)
{
  struct ct_mount_plan_output output = {{0}, 0};
  int result;
  if (stream == NULL || report == NULL ||
      ct_mount_plan_output_begin(&output) != 0) return 1;
  result = ct_mount_plan_report_write_metadata_human(stream, report, "") != 0 ||
           ct_mount_plan_report_write_entries_human(stream, report, "") != 0;
  return ct_mount_plan_output_end(stream, &output) != 0 || result != 0;
}

static int ct_mount_plan_report_json_add(
    yyjson_mut_doc *document, yyjson_mut_val *object,
    const struct ct_mount_plan_report *report)
{
  yyjson_mut_val *metadata = yyjson_mut_obj(document);
  yyjson_mut_val *entries = yyjson_mut_arr(document);
  size_t index;

  if (metadata == NULL || entries == NULL ||
      !yyjson_mut_obj_add_strcpy(document, metadata, "grammar", report->metadata.grammar) ||
      !yyjson_mut_obj_add_strcpy(document, metadata, "digest", report->metadata.digest) ||
      !yyjson_mut_obj_add_strcpy(document, metadata, "backend", report->metadata.backend) ||
      !yyjson_mut_obj_add_strcpy(document, metadata, "strategy", report->metadata.strategy) ||
      !yyjson_mut_obj_add_strcpy(document, metadata, "completeness", report->metadata.completeness) ||
      !yyjson_mut_obj_add_strcpy(document, metadata, "group_mode", report->metadata.group_mode) ||
      !yyjson_mut_obj_add_uint(document, metadata, "entry_count", report->metadata.entry_count) ||
      !yyjson_mut_obj_add_val(document, object, "metadata", metadata)) return 1;
  for (index = 0U; index < report->metadata.entry_count; ++index) {
    const struct ct_mount_plan_report_entry *source = &report->entries[index];
    yyjson_mut_val *entry = yyjson_mut_obj(document);
    char *caller = ct_mount_plan_report_escape_path(source->caller_path);
    char *target = ct_mount_plan_report_escape_path(source->target_path);
    if (entry == NULL ||
        !yyjson_mut_obj_add_strcpy(document, entry, "role", source->role) ||
        caller == NULL || target == NULL ||
        !yyjson_mut_obj_add_strcpy(document, entry, "caller_path", caller) ||
        !yyjson_mut_obj_add_strcpy(document, entry, "target_path", target) ||
        !yyjson_mut_obj_add_strcpy(document, entry, "access", source->access) ||
        !yyjson_mut_obj_add_strcpy(document, entry, "recursion", source->recursion) ||
        !yyjson_mut_arr_add_val(entries, entry)) {
      free(caller);
      free(target);
      return 1;
    }
    free(caller);
    free(target);
  }
  return yyjson_mut_obj_add_val(document, object, "entries", entries) ? 0 : 1;
}

static int ct_mount_plan_report_json_write(FILE *stream, yyjson_mut_doc *document)
{
  char *output;
  size_t length;
  int result = 1;
  output = yyjson_mut_write(document, YYJSON_WRITE_NOFLAG, &length);
  if (output != NULL && length != 0U &&
      fwrite(output, 1U, length, stream) == length &&
      fputc('\n', stream) != EOF) result = 0;
  free(output);
  return result;
}

int ct_mount_plan_report_write_json(FILE *stream,
                                    const struct ct_mount_plan_report *report)
{
  struct ct_mount_plan_output output = {{0}, 0};
  yyjson_mut_doc *document;
  yyjson_mut_val *root;
  int result = 1;
  if (stream == NULL || report == NULL ||
      ct_mount_plan_output_begin(&output) != 0) return 1;
  document = yyjson_mut_doc_new(NULL);
  root = document == NULL ? NULL : yyjson_mut_obj(document);
  if (document == NULL || root == NULL) goto done;
  yyjson_mut_doc_set_root(document, root);
  if (!yyjson_mut_obj_add_strcpy(document, root, "schema",
                                 "container-tools.mount-plan-inspect/v1") ||
      !yyjson_mut_obj_add_uint(document, root, "schema_version", 1U) ||
      !yyjson_mut_obj_add_strcpy(document, root, "path_encoding",
                                 CT_MOUNT_PLAN_PATH_ENCODING) ||
      ct_mount_plan_report_json_add(document, root, report) != 0) goto done;
  result = ct_mount_plan_report_json_write(stream, document);
done:
  yyjson_mut_doc_free(document);
  return ct_mount_plan_output_end(stream, &output) != 0 || result != 0;
}

int ct_mount_plan_report_compare_write_human(
    FILE *stream, int equal, const struct ct_mount_plan_report *left,
    const struct ct_mount_plan_report *right)
{
  struct ct_mount_plan_output output = {{0}, 0};
  int result;
  if (stream == NULL || left == NULL || right == NULL ||
      ct_mount_plan_output_begin(&output) != 0) return 1;
  result = fprintf(stream, "equal: %s\npath encoding: %s\nleft:\n",
                   equal != 0 ? "true" : "false",
                   CT_MOUNT_PLAN_PATH_ENCODING) < 0 ||
           ct_mount_plan_report_write_metadata_human(stream, left, "  ") != 0 ||
           ct_mount_plan_report_write_entries_human(stream, left, "  ") != 0 ||
           fprintf(stream, "right:\n") < 0 ||
           ct_mount_plan_report_write_metadata_human(stream, right, "  ") != 0 ||
           ct_mount_plan_report_write_entries_human(stream, right, "  ") != 0;
  return ct_mount_plan_output_end(stream, &output) != 0 || result != 0;
}

int ct_mount_plan_report_compare_write_json(
    FILE *stream, int equal, const struct ct_mount_plan_report *left,
    const struct ct_mount_plan_report *right)
{
  struct ct_mount_plan_output output = {{0}, 0};
  yyjson_mut_doc *document;
  yyjson_mut_val *root;
  yyjson_mut_val *left_object;
  yyjson_mut_val *right_object;
  int result = 1;
  if (stream == NULL || left == NULL || right == NULL ||
      ct_mount_plan_output_begin(&output) != 0) return 1;
  document = yyjson_mut_doc_new(NULL);
  root = document == NULL ? NULL : yyjson_mut_obj(document);
  left_object = document == NULL ? NULL : yyjson_mut_obj(document);
  right_object = document == NULL ? NULL : yyjson_mut_obj(document);
  if (document == NULL || root == NULL || left_object == NULL || right_object == NULL) goto done;
  yyjson_mut_doc_set_root(document, root);
  if (!yyjson_mut_obj_add_strcpy(document, root, "schema",
                                 "container-tools.mount-plan-compare/v1") ||
      !yyjson_mut_obj_add_uint(document, root, "schema_version", 1U) ||
      !yyjson_mut_obj_add_bool(document, root, "equal", equal != 0) ||
      !yyjson_mut_obj_add_strcpy(document, root, "path_encoding",
                                 CT_MOUNT_PLAN_PATH_ENCODING) ||
      ct_mount_plan_report_json_add(document, left_object, left) != 0 ||
      ct_mount_plan_report_json_add(document, right_object, right) != 0 ||
      !yyjson_mut_obj_add_val(document, root, "left", left_object) ||
      !yyjson_mut_obj_add_val(document, root, "right", right_object)) goto done;
  result = ct_mount_plan_report_json_write(stream, document);
done:
  yyjson_mut_doc_free(document);
  return ct_mount_plan_output_end(stream, &output) != 0 || result != 0;
}
