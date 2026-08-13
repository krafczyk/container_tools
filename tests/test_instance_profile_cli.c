/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "instance_profile_manifest.h"
#include "state.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CT_INSTANCE_PROFILE_CLI
#error "CT_INSTANCE_PROFILE_CLI must name the container-tools executable"
#endif

static int run_command(char *const arguments[], int close_stdout,
                       int capture_stderr, char output[16384])
{
  int pipefd[2], status;
  pid_t child;
  ssize_t received;
  size_t used = 0U;
  if (pipe(pipefd) != 0 || (child = fork()) < 0) return -1;
  if (child == 0) {
    (void)close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
        (capture_stderr != 0 && dup2(pipefd[1], STDERR_FILENO) < 0)) _exit(127);
    (void)close(pipefd[1]);
    execv(CT_INSTANCE_PROFILE_CLI, arguments);
    _exit(127);
  }
  (void)close(pipefd[1]);
  if (close_stdout != 0) (void)close(pipefd[0]);
  while (close_stdout == 0 &&
         (received = read(pipefd[0], output + used, 16383U - used)) > 0) {
    used += (size_t)received;
  }
  if (close_stdout == 0) (void)close(pipefd[0]);
  output[used] = '\0';
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

static int run(char *const arguments[], int close_stdout, char output[16384])
{
  return run_command(arguments, close_stdout, 0, output);
}

static int run_capture(char *const arguments[], char output[16384])
{
  return run_command(arguments, 0, 1, output);
}

static int write_record(const char *path,
                        const struct ct_instance_profile_manifest *profile)
{
  unsigned char *bytes = NULL;
  size_t length;
  int descriptor;
  const int failed = ct_instance_profile_manifest_serialize(profile, &bytes, &length) != 0 ||
                     (descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0 ||
                     write(descriptor, bytes, length) != (ssize_t)length ||
                     close(descriptor) != 0;
  free(bytes);
  return failed;
}

static int write_identity(const char *root, const char *name,
                          const char *profile)
{
  return ct_state_identity_write(root, name, "/image.sif", "image-id", profile);
}

int main(void)
{
  const char *groups[] = {"10"};
  char instance_name[40], unique_prefix[65], instance_prefix[33];
  char absent_instance_prefix[2];
  struct ct_instance_profile_manifest_mount mounts[] = {
      {"--bind", "/bad-\xff-\t:/data", "explicit", "inherit", "runtime-default",
       0, 1, "/bad-\xff-\t", "identity"},
  };
  struct ct_instance_profile_manifest profile = {
       "", "", instance_name, "apptainer", "ct-instance-profile-v4", "absent", "1000", "1000",
      "host", "/home/user", "/tmp/root", "/image.sif", "image-id", "absent", "absent",
      "ct-host-projection-profile-v1",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "native-inherited", groups, 1U,
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      mounts, 1U};
  char work[] = "/tmp/mkchad-v1/profile-manifest-cli.XXXXXX";
  char path[4096], explicit_prefix_record[4096], root[4096] = "", profiles[4096], digest_path[4096], temporary_path[4096], wrong_digest[65], wrong_path[4096], ambiguous_first[4096], ambiguous_second[4096], output[16384];
  char fake[4096], runtime[4096], runtime_log[4096], name_mismatch_path[4096];
  char name_mismatch_name[40];
  char *inspect[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--json", path, NULL};
  char *human[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", path, NULL};
  char *lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--json", "--ct-instance-root", root, profile.profile_digest, NULL};
  char *explicit_path[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--json", "./0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", NULL};
  char *explicit_prefix_path[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--json", "./fed", NULL};
  char *bad_root[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--ct-instance-root", "relative", profile.profile_digest, NULL};
  char *no_root[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", profile.profile_digest, NULL};
  char *wrong_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--ct-instance-root", root, wrong_digest, NULL};
  char *unique_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--json", "--ct-instance-root", root, unique_prefix, NULL};
  char *ambiguous_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--ct-instance-root", root, "fed", NULL};
  char *absent_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--ct-instance-root", root, "123", NULL};
  char *instance_inspect[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--json", "--ct-instance-root", root, instance_name, NULL};
  char *instance_short[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--ct-instance-root", root, instance_name + 7, NULL};
  char *instance_prefix_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--json", "--ct-instance-root", root, instance_prefix, NULL};
  char *instance_ambiguous_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--ct-instance-root", root, "abc", NULL};
  char *instance_absent_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--ct-instance-root", root, absent_instance_prefix, NULL};
  char *backend_inspect[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--json", "--apptainer", instance_name, NULL};
  char *backend_prefix_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--json", "--apptainer", instance_prefix, NULL};
  char *backend_ambiguous_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--apptainer", "abc", NULL};
  char *backend_absent_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--apptainer", absent_instance_prefix, NULL};

  if (ct_instance_profile_manifest_profile_digest(&profile,
                                                    profile.profile_digest) != 0 ||
      snprintf(instance_name, sizeof(instance_name), "mkchad-%.32s",
               profile.profile_digest) >= (int)sizeof(instance_name) ||
      mkdtemp(work) == NULL ||
      snprintf(path, sizeof(path), "%s/record", work) >= (int)sizeof(path) ||
      snprintf(explicit_prefix_record, sizeof(explicit_prefix_record),
               "%s/fed", work) >= (int)sizeof(explicit_prefix_record) ||
      snprintf(root, sizeof(root), "%s/root", work) >= (int)sizeof(root) ||
      snprintf(profiles, sizeof(profiles), "%s/profiles", root) >= (int)sizeof(profiles) ||
      snprintf(digest_path, sizeof(digest_path), "%s/%s.manifest", profiles, profile.profile_digest) >= (int)sizeof(digest_path) ||
      snprintf(temporary_path, sizeof(temporary_path), "%s/.tmp.%s.stale0",
               profiles, profile.profile_digest) >= (int)sizeof(temporary_path) ||
      snprintf(wrong_digest, sizeof(wrong_digest), "%064d", 1) >= (int)sizeof(wrong_digest) ||
        snprintf(wrong_path, sizeof(wrong_path), "%s/%s.manifest", profiles, wrong_digest) >= (int)sizeof(wrong_path) ||
        snprintf(ambiguous_first, sizeof(ambiguous_first), "%s/%s.manifest", profiles,
                 "fed0000000000000000000000000000000000000000000000000000000000000") >= (int)sizeof(ambiguous_first) ||
       snprintf(ambiguous_second, sizeof(ambiguous_second), "%s/%s.manifest", profiles,
                 "fed1111111111111111111111111111111111111111111111111111111111111") >= (int)sizeof(ambiguous_second) ||
       write_record(path, &profile) != 0 ||
       write_record(explicit_prefix_record, &profile) != 0 ||
       mkdir(root, 0700) != 0 || mkdir(profiles, 0700) != 0 ||
       link(path, digest_path) != 0 || link(path, temporary_path) != 0 ||
       chdir(work) != 0) {
    return 1;
  }
  memcpy(unique_prefix, profile.profile_digest, 3U);
  unique_prefix[3] = '\0';
  memcpy(instance_prefix, profile.profile_digest, 3U);
  instance_prefix[3] = '\0';
  absent_instance_prefix[0] = profile.profile_digest[0] == '0' ? '1' : '0';
  absent_instance_prefix[1] = '\0';
  if (write_identity(root, instance_name, profile.profile_digest) != 0) return 1;
  if (run(inspect, 0, output) != 0) { (void)fprintf(stderr, "inspect %s\n", output); return 1; }
  if (strstr(output, "container-tools.instance-profile-inspect/v1") == NULL || strstr(output, "\\xff") == NULL) { (void)fprintf(stderr, "json %s\n", output); return 1; }
  if (run(human, 0, output) != 0 || strstr(output, "\\xff") == NULL) { (void)fprintf(stderr, "human %s\n", output); return 1; }
  if (run(lookup, 0, output) != 0) { (void)fprintf(stderr, "lookup %s\n", output); return 1; }
  if (run(unique_lookup, 0, output) != 0 ||
      strstr(output, "container-tools.instance-profile-inspect/v1") == NULL) {
    (void)fprintf(stderr, "unique selector %s\n", output); return 1;
  }
  if (link(path, ambiguous_first) != 0 || link(path, ambiguous_second) != 0 ||
      run_capture(ambiguous_lookup, output) != 125 ||
      strstr(output, "ambiguous") == NULL || strstr(output, root) != NULL ||
      run_capture(absent_lookup, output) != 125 ||
      strstr(output, root) != NULL) {
    (void)fprintf(stderr, "prefix selector %s\n", output); return 1;
  }
  if (link(path, wrong_path) != 0 || run(wrong_lookup, 0, output) != 125) { (void)fprintf(stderr, "wrong selector\n"); return 1; }
  if (run(instance_inspect, 0, output) != 0 || strstr(output, "container-tools.instance-profile-inspect/v1") == NULL) { (void)fprintf(stderr, "instance inspect %s\n", output); return 1; }
  if (run(instance_short, 0, output) != 0 || strstr(output, "instance name:") == NULL) { (void)fprintf(stderr, "instance short %s\n", output); return 1; }
  if (run(instance_prefix_lookup, 0, output) != 0 ||
      strstr(output, "container-tools.instance-profile-inspect/v1") == NULL ||
      run_capture(instance_absent_lookup, output) != 125 ||
      strstr(output, root) != NULL ||
      write_identity(root, "mkchad-abc00000000000000000000000000000",
                     "abc00000000000000000000000000000abc00000000000000000000000000000") != 0 ||
      write_identity(root, "mkchad-abc11111111111111111111111111111",
                     "abc11111111111111111111111111111abc11111111111111111111111111111") != 0 ||
      run_capture(instance_ambiguous_lookup, output) != 125 ||
      strstr(output, "ambiguous") == NULL || strstr(output, root) != NULL) {
    (void)fprintf(stderr, "instance prefix %s\n", output); return 1;
  }
  if (run(instance_inspect, 1, output) != 125) return 1;
  if (setenv("CT_TEST_STORAGE_PRESENT_FOREIGN_UID", "1", 1) != 0 ||
      chmod(root, 0755) != 0 || run(instance_inspect, 0, output) != 0 ||
       chmod(root, 0700) != 0) return 1;
  if (chmod(profiles, 0755) != 0 || run(instance_inspect, 0, output) != 0 ||
       chmod(profiles, 0700) != 0) return 1;
  {
    char identity[4096];
    if (snprintf(identity, sizeof(identity), "%s/%s.identity", root, instance_name) >= (int)sizeof(identity) ||
        chmod(identity, 0644) != 0 || run(instance_inspect, 0, output) != 0 ||
        chmod(identity, 0600) != 0 || chmod(digest_path, 0644) != 0 ||
        run(instance_inspect, 0, output) != 0 || chmod(digest_path, 0600) != 0 ||
        unsetenv("CT_TEST_STORAGE_PRESENT_FOREIGN_UID") != 0) return 1;
  }
  if (run(no_root, 0, output) != 64) { (void)fprintf(stderr, "no-root\n"); return 1; }
  if (run(bad_root, 0, output) != 64) { (void)fprintf(stderr, "bad-root\n"); return 1; }
  if (run(explicit_path, 0, output) != 125) { (void)fprintf(stderr, "explicit\n"); return 1; }
  if (run(explicit_prefix_path, 0, output) != 0 ||
      strstr(output, "container-tools.instance-profile-inspect/v1") == NULL) {
    (void)fprintf(stderr, "explicit prefix %s\n", output); return 1;
  }
  if (run(inspect, 1, output) != 125) { (void)fprintf(stderr, "closed\n"); return 1; }
  if (snprintf(fake, sizeof(fake), "%s/fake", work) >= (int)sizeof(fake) ||
      snprintf(runtime, sizeof(runtime), "%s/apptainer", fake) >= (int)sizeof(runtime) ||
      snprintf(runtime_log, sizeof(runtime_log), "%s/runtime.log", work) >= (int)sizeof(runtime_log) ||
      snprintf(name_mismatch_path, sizeof(name_mismatch_path), "%s/name-mismatch", work) >= (int)sizeof(name_mismatch_path) ||
      mkdir(fake, 0700) != 0) return 1;
  {
    struct ct_instance_profile_manifest alternate = profile;
    FILE *stream;
    alternate.backend = "singularity";
    alternate.instance_name = name_mismatch_name;
    if (ct_instance_profile_manifest_profile_digest(
            &alternate, alternate.profile_digest) != 0 ||
        snprintf(name_mismatch_name, sizeof(name_mismatch_name), "mkchad-%.32s",
                 alternate.profile_digest) >= (int)sizeof(name_mismatch_name) ||
        write_record(name_mismatch_path, &alternate) != 0) return 1;
    if ((stream = fopen(runtime, "w")) == NULL ||
        fputs("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$CT_INSTANCE_PROFILE_RUNTIME_LOG\"\ncase \" $* \" in\n  *' instance list --json '*)\n    case \"$CT_INSTANCE_PROFILE_RUNTIME_MODE\" in\n      prefix) printf '{\"instances\":[{\"instance\":\"%s\"}]}\\n' \"$CT_INSTANCE_PROFILE_RUNTIME_NAME\" ;;\n      duplicate) printf '{\"instances\":[{\"instance\":\"%s\"},{\"instance\":\"%s\"}]}\\n' \"$CT_INSTANCE_PROFILE_RUNTIME_NAME\" \"$CT_INSTANCE_PROFILE_RUNTIME_NAME\" ;;\n      ambiguous) printf '{\"instances\":[{\"instance\":\"mkchad-abc00000000000000000000000000000\"},{\"instance\":\"mkchad-abc11111111111111111111111111111\"}]}\\n' ;;\n      mixed-malformed) printf '{\"instances\":[{\"instance\":\"%s\"},{\"name\":\"mkchad-abc11111111111111111111111111111\"}]}\\n' \"$CT_INSTANCE_PROFILE_RUNTIME_NAME\" ;;\n      absent) printf '{\"instances\":[]}\\n' ;;\n      list-malformed) printf malformed ;;\n      list-failure) exit 42 ;;\n      interrupt) kill -INT $$ ;;\n      terminate) kill -TERM $$ ;;\n      *) exit 42 ;;\n    esac\n    exit 0 ;;\nesac\ncase \"$CT_INSTANCE_PROFILE_RUNTIME_MODE\" in\n  valid|prefix|duplicate) exec /bin/cat \"$CT_INSTANCE_PROFILE_RUNTIME_RECORD\" ;;\n  name) exec /bin/cat \"$CT_INSTANCE_PROFILE_RUNTIME_NAME_RECORD\" ;;\n  malformed) printf malformed ;;\n  oversized) /usr/bin/head -c 1048577 /dev/zero ;;\n  failure) exit 42 ;;\nesac\n", stream) == EOF ||
        fclose(stream) != 0 || chmod(runtime, 0700) != 0 || setenv("HOME", work, 1) != 0 ||
      setenv("PATH", fake, 1) != 0 || setenv("CT_INSTANCE_PROFILE_RUNTIME_LOG", runtime_log, 1) != 0 ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_RECORD", path, 1) != 0 ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_NAME", instance_name, 1) != 0 ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_NAME_RECORD", name_mismatch_path, 1) != 0) return 1;
  }
  if (setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "valid", 1) != 0 ||
      setenv("CT_SINGULARITY_ARGS", "--quiet", 1) != 0 ||
      run(backend_inspect, 0, output) != 0 ||
      strstr(output, "container-tools.instance-profile-inspect/v1") == NULL) {
    (void)fprintf(stderr, "backend valid: %s\n", output);
    return 1;
  }
  {
    FILE *stream = fopen(runtime_log, "r");
    size_t used;
    if (stream == NULL || (used = fread(output, 1U, sizeof(output) - 1U, stream)) == 0U ||
        fclose(stream) != 0) return 1;
    output[used] = '\0';
    {
      char expected[256];
      if (snprintf(expected, sizeof(expected),
                   "--quiet\nexec\ninstance://%s\n/bin/cat\n/.container-tools-instance-profile\n",
                   instance_name) >= (int)sizeof(expected) ||
          strcmp(output, expected) != 0) return 1;
    }
  }
  if (unsetenv("CT_SINGULARITY_ARGS") != 0) return 1;
  if (setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "prefix", 1) != 0 ||
      run(backend_prefix_lookup, 0, output) != 0 ||
      strstr(output, "container-tools.instance-profile-inspect/v1") == NULL ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "duplicate", 1) != 0 ||
      run(backend_prefix_lookup, 0, output) != 0 ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "absent", 1) != 0 ||
      run(backend_absent_lookup, 0, output) != 125 ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "ambiguous", 1) != 0 ||
      run_capture(backend_ambiguous_lookup, output) != 125 ||
      strstr(output, "ambiguous") == NULL ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "mixed-malformed", 1) != 0 ||
      run(backend_prefix_lookup, 0, output) != 125 ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "list-malformed", 1) != 0 ||
      run(backend_prefix_lookup, 0, output) != 125 ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "list-failure", 1) != 0 ||
      run(backend_prefix_lookup, 0, output) != 125 ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "interrupt", 1) != 0 ||
      run(backend_prefix_lookup, 0, output) != 128 + SIGINT ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "terminate", 1) != 0 ||
      run(backend_prefix_lookup, 0, output) != 128 + SIGTERM) return 1;
  if (setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "failure", 1) != 0 ||
      run(backend_inspect, 0, output) != 125 || output[0] != '\0' ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "malformed", 1) != 0 ||
      run(backend_inspect, 0, output) != 125 || output[0] != '\0' ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "name", 1) != 0 ||
      run(backend_inspect, 0, output) != 125 || output[0] != '\0' ||
      setenv("CT_INSTANCE_PROFILE_RUNTIME_MODE", "oversized", 1) != 0 ||
      run(backend_inspect, 0, output) != 125 || output[0] != '\0') return 1;
  return 0;
}
