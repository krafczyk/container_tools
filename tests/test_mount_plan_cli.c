/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "mount_plan.h"
#include "mount_plan_report.h"

#include "yyjson.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CT_MOUNT_PLAN_CLI
#error "CT_MOUNT_PLAN_CLI must name the container-tools executable"
#endif

struct command_result {
  char stdout_text[32768];
  char stderr_text[32768];
  size_t stdout_length;
  size_t stderr_length;
};

struct command_options {
  const char *default_path;
  int fail_allocations;
  int closed_stdout;
  int shared_output;
};

#define ARRAY_LENGTH(values) (sizeof(values) / sizeof((values)[0]))

static int write_plan(const char *path, const struct ct_mount_plan *plan)
{
  unsigned char *bytes = NULL;
  char digest[65];
  size_t length;
  int descriptor;
  int result;

  if (ct_mount_plan_serialize(plan, &bytes, &length, digest) != 0) return 1;
  descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  result = descriptor < 0 || write(descriptor, bytes, length) != (ssize_t)length ||
           close(descriptor) != 0;
  free(bytes);
  return result;
}

static int read_all(int descriptor, char *output, size_t output_size,
                    size_t *length, int *complete)
{
  ssize_t received;
  if (descriptor < 0 || output_size == 0U || length == NULL || complete == NULL) {
    return 1;
  }
  received = read(descriptor, output + *length, output_size - 1U - *length);
  if (received > 0) {
    *length += (size_t)received;
    output[*length] = '\0';
    return *length == output_size - 1U ? 1 : 0;
  }
  if (received < 0 && errno == EINTR) return 0;
  if (received < 0 || close(descriptor) != 0) return 1;
  *complete = 1;
  return 0;
}

static int capture_output(int stdout_descriptor, int stderr_descriptor,
                          struct command_result *result)
{
  struct pollfd descriptors[2] = {
      {stdout_descriptor, POLLIN, 0}, {stderr_descriptor, POLLIN, 0}};
  int stdout_complete = stdout_descriptor < 0;
  int stderr_complete = stderr_descriptor < 0;
  result->stdout_length = 0U;
  result->stderr_length = 0U;
  result->stdout_text[0] = '\0';
  result->stderr_text[0] = '\0';
  while (stdout_complete == 0 || stderr_complete == 0) {
    int ready = poll(descriptors, 2U, -1);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) return 1;
    if (stdout_complete == 0 && descriptors[0].revents != 0 &&
        read_all(stdout_descriptor, result->stdout_text,
                 sizeof(result->stdout_text), &result->stdout_length,
                 &stdout_complete) != 0) return 1;
    if (stderr_complete == 0 && descriptors[1].revents != 0 &&
        read_all(stderr_descriptor, result->stderr_text,
                 sizeof(result->stderr_text), &result->stderr_length,
                 &stderr_complete) != 0) return 1;
  }
  return 0;
}

static int run_command(char *const arguments[],
                       const struct command_options *options,
                       struct command_result *result)
{
  int stdout_pipe[2] = {-1, -1}, stderr_pipe[2] = {-1, -1};
  pid_t child = -1;
  int status;

  if (pipe(stdout_pipe) != 0) return -1;
  if (pipe(stderr_pipe) != 0) {
    (void)close(stdout_pipe[0]);
    (void)close(stdout_pipe[1]);
    return -1;
  }
  child = fork();
  if (child == 0) {
    (void)close(stdout_pipe[0]);
    (void)close(stderr_pipe[0]);
    if (options != NULL && options->default_path != NULL &&
        setenv("CT_MOUNT_PLAN_TEST_DEFAULT_PATH", options->default_path, 1) != 0) {
      _exit(127);
    }
    if (options != NULL && options->fail_allocations != 0 &&
        setenv("CT_MOUNT_PLAN_REPORT_TEST_FAIL_ALLOCATIONS", "1", 1) != 0) {
      _exit(127);
    }
    if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(options != NULL && options->shared_output != 0
                 ? stdout_pipe[1] : stderr_pipe[1], STDERR_FILENO) < 0) _exit(127);
    (void)close(stdout_pipe[1]);
    (void)close(stderr_pipe[1]);
    execv(CT_MOUNT_PLAN_CLI, arguments);
    _exit(127);
  }
  (void)close(stdout_pipe[1]);
  (void)close(stderr_pipe[1]);
  stdout_pipe[1] = -1;
  stderr_pipe[1] = -1;
  if (child < 0) {
    (void)close(stdout_pipe[0]);
    (void)close(stderr_pipe[0]);
    return -1;
  }
  if (options != NULL && options->shared_output != 0) {
    (void)close(stderr_pipe[0]);
    stderr_pipe[0] = -1;
  }
  if (options != NULL && options->closed_stdout != 0) {
    (void)close(stdout_pipe[0]);
    stdout_pipe[0] = -1;
  }
  if (capture_output(stdout_pipe[0], stderr_pipe[0], result) != 0) {
    if (stdout_pipe[0] >= 0) (void)close(stdout_pipe[0]);
    if (stderr_pipe[0] >= 0) (void)close(stderr_pipe[0]);
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
    return -1;
  }
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

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

static int validate_plan_json(yyjson_val *value, const char *caller_path,
                              size_t entry_count)
{
  static const char *const metadata_keys[] = {
      "grammar", "digest", "backend", "strategy", "completeness", "group_mode",
      "entry_count"};
  static const char *const entry_keys[] = {
      "role", "caller_path", "target_path", "access", "recursion"};
  yyjson_val *metadata = yyjson_obj_get(value, "metadata");
  yyjson_val *entries = yyjson_obj_get(value, "entries");
  size_t index;
  if (object_keys(metadata, metadata_keys, ARRAY_LENGTH(metadata_keys)) != 0 ||
      !yyjson_equals_str(yyjson_obj_get(metadata, "grammar"), "ct-mount-plan-v1") ||
      !yyjson_is_str(yyjson_obj_get(metadata, "digest")) ||
      !yyjson_equals_str(yyjson_obj_get(metadata, "backend"), "docker") ||
      !yyjson_equals_str(yyjson_obj_get(metadata, "strategy"), "none") ||
      !yyjson_equals_str(yyjson_obj_get(metadata, "completeness"), "complete") ||
      !yyjson_equals_str(yyjson_obj_get(metadata, "group_mode"), "primary-only") ||
      !yyjson_is_uint(yyjson_obj_get(metadata, "entry_count")) ||
      yyjson_get_uint(yyjson_obj_get(metadata, "entry_count")) != entry_count ||
      !yyjson_is_arr(entries) || yyjson_arr_size(entries) != entry_count) return 1;
  for (index = 0U; index < entry_count; ++index) {
    yyjson_val *entry = yyjson_arr_get(entries, index);
    if (object_keys(entry, entry_keys, ARRAY_LENGTH(entry_keys)) != 0 ||
        !yyjson_is_str(yyjson_obj_get(entry, "role")) ||
        !yyjson_is_str(yyjson_obj_get(entry, "caller_path")) ||
        !yyjson_is_str(yyjson_obj_get(entry, "target_path")) ||
        !yyjson_is_str(yyjson_obj_get(entry, "access")) ||
        !yyjson_is_str(yyjson_obj_get(entry, "recursion"))) return 1;
  }
  return strcmp(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(entries, 0U),
                                               "caller_path")), caller_path) != 0;
}

static int validate_inspect_json(const struct command_result *result,
                                 const char *caller_path, size_t entry_count)
{
  static const char *const root_keys[] = {
      "schema", "schema_version", "path_encoding", "metadata", "entries"};
  yyjson_doc *document;
  yyjson_val *root;
  int invalid;
  if (result->stdout_length == 0U || result->stdout_text[result->stdout_length - 1U] != '\n') return 1;
  document = yyjson_read(result->stdout_text, result->stdout_length - 1U,
                         YYJSON_READ_NOFLAG);
  if (document == NULL) return 1;
  root = yyjson_doc_get_root(document);
  invalid = object_keys(root, root_keys, ARRAY_LENGTH(root_keys)) != 0 ||
            !yyjson_equals_str(yyjson_obj_get(root, "schema"),
                               "container-tools.mount-plan-inspect/v1") ||
            !yyjson_is_uint(yyjson_obj_get(root, "schema_version")) ||
            yyjson_get_uint(yyjson_obj_get(root, "schema_version")) != 1U ||
            !yyjson_equals_str(yyjson_obj_get(root, "path_encoding"),
                               "backslash-escaped-bytes") ||
            validate_plan_json(root, caller_path, entry_count) != 0;
  yyjson_doc_free(document);
  return invalid;
}

static int validate_compare_json(const struct command_result *result,
                                 int equal, const char *right_path,
                                 size_t right_entry_count)
{
  static const char *const root_keys[] = {
      "schema", "schema_version", "equal", "path_encoding", "left", "right"};
  yyjson_doc *document;
  yyjson_val *root;
  int invalid;
  if (result->stdout_length == 0U || result->stdout_text[result->stdout_length - 1U] != '\n') return 1;
  document = yyjson_read(result->stdout_text, result->stdout_length - 1U,
                         YYJSON_READ_NOFLAG);
  if (document == NULL) return 1;
  root = yyjson_doc_get_root(document);
  invalid = object_keys(root, root_keys, ARRAY_LENGTH(root_keys)) != 0 ||
            !yyjson_equals_str(yyjson_obj_get(root, "schema"),
                               "container-tools.mount-plan-compare/v1") ||
            !yyjson_is_uint(yyjson_obj_get(root, "schema_version")) ||
            yyjson_get_uint(yyjson_obj_get(root, "schema_version")) != 1U ||
            !yyjson_is_bool(yyjson_obj_get(root, "equal")) ||
            yyjson_get_bool(yyjson_obj_get(root, "equal")) != (equal != 0) ||
            !yyjson_equals_str(yyjson_obj_get(root, "path_encoding"),
                               "backslash-escaped-bytes") ||
            validate_plan_json(yyjson_obj_get(root, "left"),
                               "/bad-\\xff-\\x09-\\\\-\\\"", 2U) != 0 ||
            validate_plan_json(yyjson_obj_get(root, "right"), right_path,
                               right_entry_count) != 0;
  yyjson_doc_free(document);
  return invalid;
}

int main(void)
{
  const char escaped_path[] = "/bad-\\xff-\\x09-\\\\-\\\"";
  const char raw_path[] = "/bad-\xff-\t-\\-\"";
  const struct ct_mount_plan_entry entries[] = {
    {"explicit", raw_path, raw_path, "inherit", "runtime-default"},
    {"bootstrap-internal", "/.container-tools-bootstrap",
     "/.container-tools-bootstrap", "read-only", "runtime-default"},
  };
  const struct ct_mount_plan_entry changed_entries[] = {
    {"explicit", "/other", "/other", "read-only", "runtime-default"},
  };
  const struct ct_mount_plan plan = {
    "docker", "none", "complete", "primary-only", entries, 2U};
  const struct ct_mount_plan changed = {
    "docker", "none", "complete", "primary-only", changed_entries, 1U};
  char work[] = "/tmp/mkchad-v1/container-tools-c11/mount-plan-cli.XXXXXX";
  char left[4096], right[4096], option_path[4096], malformed[4096], missing[4096], cache[4096], cache_locks[4096], cache_manifest[4096], unexpected[4096];
  char *inspect_default[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "inspect", "--json", NULL};
  char *inspect_human[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "inspect", left, NULL};
  char *inspect_json[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "inspect", "--json", left, NULL};
  char *inspect_option[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "inspect", "--json", "--", "--option", NULL};
  char *compare_option[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "compare", "--json", "--", "--option", "--option", NULL};
  char *compare_default[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "compare", "--json", left, NULL};
  char *compare_equal[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "compare", left, right, NULL};
  char *compare_json[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "compare", "--json", left, right, NULL};
  char *compare_different[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "compare", "--json", left, right, NULL};
  char *inspect_malformed[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "inspect", "--json", malformed, NULL};
  char *inspect_missing[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "inspect", "--json", missing, NULL};
  char *compare_malformed[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "compare", "--json", malformed, left, NULL};
  char *usage[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "inspect", "--help", NULL};
  char *clear[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "clear", NULL};
  char *clear_help[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "clear", "--help", NULL};
  char *clear_invalid[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "clear", "/tmp", NULL};
  char *invalid[] = {CT_MOUNT_PLAN_CLI, "mount", "plan", "compare", "--json", "--json", left, NULL};
  struct command_result result;
  const struct command_options default_right = {right, 0, 0, 0};
  const struct command_options default_left_fail_allocations = {left, 1, 0, 0};
  const struct command_options closed_stdout = {NULL, 0, 1, 0};
  const struct command_options shared_closed_output = {NULL, 0, 1, 1};
  struct ct_mount_plan_report report;
  int descriptor;

  if (mkdtemp(work) == NULL ||
      snprintf(left, sizeof(left), "%s/left", work) >= (int)sizeof(left) ||
      snprintf(right, sizeof(right), "%s/right", work) >= (int)sizeof(right) ||
      snprintf(option_path, sizeof(option_path), "%s/--option", work) >= (int)sizeof(option_path) ||
      snprintf(malformed, sizeof(malformed), "%s/malformed", work) >= (int)sizeof(malformed) ||
      snprintf(missing, sizeof(missing), "%s/missing", work) >= (int)sizeof(missing) ||
      snprintf(cache, sizeof(cache), "%s/cache", work) >= (int)sizeof(cache) ||
      snprintf(cache_locks, sizeof(cache_locks), "%s/.locks", cache) >= (int)sizeof(cache_locks) ||
      snprintf(cache_manifest, sizeof(cache_manifest), "%s/%064d.manifest", cache, 0) >= (int)sizeof(cache_manifest) ||
      snprintf(unexpected, sizeof(unexpected), "%s/unexpected", cache) >= (int)sizeof(unexpected) ||
      write_plan(left, &plan) != 0 || write_plan(right, &plan) != 0 ||
      write_plan(option_path, &plan) != 0 || chdir(work) != 0) return 1;

  ct_mount_plan_report_init(&report);
  if (ct_mount_plan_report_read(left, &report) != CT_MOUNT_PLAN_READ_OK ||
      ct_mount_plan_report_read(right, &report) != CT_MOUNT_PLAN_READ_OK ||
      report.metadata.entry_count != 2U ||
      strcmp(report.entries[0].caller_path, raw_path) != 0) {
    (void)fprintf(stderr, "report reread failed\n");
    return 1;
  }
  ct_mount_plan_report_destroy(&report);

  if (run_command(inspect_default, &default_right, &result) != 0 ||
      result.stderr_length != 0U || validate_inspect_json(&result, escaped_path, 2U) != 0) {
    (void)fprintf(stderr, "default inspect failed\n");
    return 1;
  }
  if (run_command(inspect_human, NULL, &result) != 0 ||
      result.stderr_length != 0U || strstr(result.stdout_text, escaped_path) == NULL ||
      strstr(result.stdout_text, raw_path) != NULL) {
    (void)fprintf(stderr, "human inspect failed\n");
    return 1;
  }
  if (run_command(inspect_option, NULL, &result) != 0 ||
      result.stderr_length != 0U || validate_inspect_json(&result, escaped_path, 2U) != 0) {
    (void)fprintf(stderr, "option path failed\n");
    return 1;
  }
  if (run_command(compare_option, NULL, &result) != 0 ||
      result.stderr_length != 0U || validate_compare_json(&result, 1, escaped_path, 2U) != 0) {
    (void)fprintf(stderr, "option comparison failed\n");
    return 1;
  }
  if (run_command(compare_equal, NULL, &result) != 0 ||
      result.stderr_length != 0U || strstr(result.stdout_text, "equal: true\n") == NULL) {
    (void)fprintf(stderr, "success report failed\n");
    return 1;
  }

  if (unlink(right) != 0 || write_plan(right, &changed) != 0 ||
      run_command(compare_default, &default_right, &result) != 1 ||
      result.stderr_length != 0U || validate_compare_json(&result, 0, "/other", 1U) != 0 ||
      run_command(compare_different, NULL, &result) != 1 ||
      result.stderr_length != 0U || validate_compare_json(&result, 0, "/other", 1U) != 0) {
    (void)fprintf(stderr, "comparison report failed\n");
    return 1;
  }

  descriptor = open(malformed, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0 || write(descriptor, "bad", 3U) != 3 || close(descriptor) != 0) return 1;
  if (run_command(inspect_malformed, NULL, &result) != 125 ||
      result.stdout_length != 0U || result.stderr_length == 0U) {
    (void)fprintf(stderr, "malformed failed\n");
    return 1;
  }
  if (run_command(inspect_missing, NULL, &result) != 125 ||
      result.stdout_length != 0U || result.stderr_length == 0U) {
    (void)fprintf(stderr, "missing failed\n");
    return 1;
  }
  if (run_command(compare_malformed, NULL, &result) != 125 ||
      result.stdout_length != 0U || result.stderr_length == 0U) {
    (void)fprintf(stderr, "compare malformed failed\n");
    return 1;
  }
  if (run_command(inspect_default, &default_left_fail_allocations, &result) != 125 ||
      result.stdout_length != 0U || result.stderr_length == 0U) {
    (void)fprintf(stderr, "allocation failed\n");
    return 1;
  }
  if (run_command(inspect_human, &closed_stdout, &result) != 125 ||
      result.stderr_length == 0U) {
    (void)fprintf(stderr, "closed stdout failed\n");
    return 1;
  }
  if (run_command(inspect_json, &closed_stdout, &result) != 125 ||
      result.stderr_length == 0U) {
    (void)fprintf(stderr, "closed JSON inspect failed\n");
    return 1;
  }
  if (run_command(compare_equal, &closed_stdout, &result) != 125 ||
      result.stderr_length == 0U) {
    (void)fprintf(stderr, "closed human comparison failed\n");
    return 1;
  }
  if (run_command(compare_json, &closed_stdout, &result) != 125 ||
      result.stderr_length == 0U) {
    (void)fprintf(stderr, "closed JSON comparison failed\n");
    return 1;
  }
  if (run_command(usage, &closed_stdout, &result) != 125 ||
      result.stderr_length == 0U) {
    (void)fprintf(stderr, "closed usage failed\n");
    return 1;
  }
  if (run_command(inspect_json, &shared_closed_output, &result) != 125) {
    (void)fprintf(stderr, "shared closed output terminated on SIGPIPE\n");
    return 1;
  }
  if (run_command(usage, NULL, &result) != 0 || result.stderr_length != 0U ||
      strcmp(result.stdout_text, "usage: container-tools mount plan inspect [--json] [--] [PATH]\n") != 0) {
    (void)fprintf(stderr, "usage failed\n");
    return 1;
  }
  if (setenv("CT_MOUNT_PLAN_STATE_ROOT", cache, 1) != 0 ||
      run_command(clear, NULL, &result) != 0 || result.stdout_length != 0U ||
      result.stderr_length != 0U || mkdir(cache, 0700) != 0 ||
      mkdir(cache_locks, 0700) != 0 || write_plan(cache_manifest, &plan) != 0 ||
      run_command(clear, NULL, &result) != 0 || result.stdout_length != 0U ||
      result.stderr_length != 0U || access(cache_manifest, F_OK) == 0 ||
      run_command(clear, NULL, &result) != 0 ||
      run_command(clear_help, NULL, &result) != 0 || result.stderr_length != 0U ||
      strcmp(result.stdout_text, "usage: container-tools mount plan clear [--help]\n") != 0 ||
      run_command(clear_help, &closed_stdout, &result) != 125 ||
      result.stderr_length == 0U ||
      (descriptor = open(unexpected, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0 ||
      close(descriptor) != 0 || run_command(clear, NULL, &result) != 125 ||
      result.stdout_length != 0U || result.stderr_length == 0U ||
      strstr(result.stderr_text, cache) != NULL || access(unexpected, F_OK) != 0 ||
      run_command(clear_invalid, NULL, &result) != 64 ||
      result.stdout_length != 0U || result.stderr_length == 0U ||
      strstr(result.stderr_text, cache) != NULL ||
      unlink(unexpected) != 0 || symlink(left, cache_manifest) != 0 ||
      run_command(clear, NULL, &result) != 125 || access(cache_manifest, F_OK) != 0 ||
      unlink(cache_manifest) != 0 || write_plan(cache_manifest, &plan) != 0 ||
      chmod(cache_manifest, 0644) != 0 || run_command(clear, NULL, &result) != 125 ||
      access(cache_manifest, F_OK) != 0 || unlink(cache_manifest) != 0 ||
      unsetenv("CT_MOUNT_PLAN_STATE_ROOT") != 0) {
    (void)fprintf(stderr, "clear command failed\n");
    return 1;
  }
  if (run_command(invalid, NULL, &result) != 64 ||
      result.stdout_length != 0U || result.stderr_length == 0U) {
    (void)fprintf(stderr, "invalid command failed\n");
    return 1;
  }
  return 0;
}
