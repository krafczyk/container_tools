/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "instance_profile_manifest.h"

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

static int run(char *const arguments[], int close_stdout, char output[16384])
{
  int pipefd[2], status;
  pid_t child;
  ssize_t received;
  size_t used = 0U;
  if (pipe(pipefd) != 0 || (child = fork()) < 0) return -1;
  if (child == 0) {
    (void)close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
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

int main(void)
{
  const char *groups[] = {"10"};
  char instance_name[40];
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
  char path[4096], root[4096] = "", profiles[4096], digest_path[4096], wrong_digest[65], wrong_path[4096], output[16384];
  char fake[4096], runtime[4096], runtime_log[4096], name_mismatch_path[4096];
  char name_mismatch_name[40];
  char *inspect[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--json", path, NULL};
  char *human[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", path, NULL};
  char *lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--json", "--ct-instance-root", root, profile.profile_digest, NULL};
  char *explicit_path[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--json", "./0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", NULL};
  char *bad_root[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--ct-instance-root", "relative", profile.profile_digest, NULL};
  char *no_root[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", profile.profile_digest, NULL};
  char *wrong_lookup[] = {CT_INSTANCE_PROFILE_CLI, "instance", "profile", "inspect", "--ct-instance-root", root, wrong_digest, NULL};
  char *instance_inspect[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--json", "--ct-instance-root", root, instance_name, NULL};
  char *instance_short[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--ct-instance-root", root, instance_name + 7, NULL};
  char *backend_inspect[] = {CT_INSTANCE_PROFILE_CLI, "instance", "inspect", "--json", "--apptainer", instance_name, NULL};

  if (ct_instance_profile_manifest_profile_digest(&profile,
                                                   profile.profile_digest) != 0 ||
      snprintf(instance_name, sizeof(instance_name), "mkchad-%.32s",
               profile.profile_digest) >= (int)sizeof(instance_name) ||
      mkdtemp(work) == NULL ||
      snprintf(path, sizeof(path), "%s/record", work) >= (int)sizeof(path) ||
      snprintf(root, sizeof(root), "%s/root", work) >= (int)sizeof(root) ||
      snprintf(profiles, sizeof(profiles), "%s/profiles", root) >= (int)sizeof(profiles) ||
      snprintf(digest_path, sizeof(digest_path), "%s/%s.manifest", profiles, profile.profile_digest) >= (int)sizeof(digest_path) ||
      snprintf(wrong_digest, sizeof(wrong_digest), "%064d", 1) >= (int)sizeof(wrong_digest) ||
       snprintf(wrong_path, sizeof(wrong_path), "%s/%s.manifest", profiles, wrong_digest) >= (int)sizeof(wrong_path) ||
       write_record(path, &profile) != 0 ||
       mkdir(root, 0700) != 0 || mkdir(profiles, 0700) != 0 ||
       link(path, digest_path) != 0 || chdir(work) != 0) {
    return 1;
  }
  {
    char identity[4096];
    FILE *stream;
    if (snprintf(identity, sizeof(identity), "%s/%s.identity", root, instance_name) >= (int)sizeof(identity) ||
        (stream = fopen(identity, "w")) == NULL ||
        fprintf(stream, "name=%s\nimage=/image.sif\nidentity=image-id\nprofile=%s\n", instance_name, profile.profile_digest) < 0 ||
        fclose(stream) != 0 || chmod(identity, 0600) != 0) return 1;
  }
  if (run(inspect, 0, output) != 0) { (void)fprintf(stderr, "inspect %s\n", output); return 1; }
  if (strstr(output, "container-tools.instance-profile-inspect/v1") == NULL || strstr(output, "\\xff") == NULL) { (void)fprintf(stderr, "json %s\n", output); return 1; }
  if (run(human, 0, output) != 0 || strstr(output, "\\xff") == NULL) { (void)fprintf(stderr, "human %s\n", output); return 1; }
  if (run(lookup, 0, output) != 0) { (void)fprintf(stderr, "lookup %s\n", output); return 1; }
  if (link(path, wrong_path) != 0 || run(wrong_lookup, 0, output) != 125) { (void)fprintf(stderr, "wrong selector\n"); return 1; }
  if (run(instance_inspect, 0, output) != 0 || strstr(output, "container-tools.instance-profile-inspect/v1") == NULL) { (void)fprintf(stderr, "instance inspect %s\n", output); return 1; }
  if (run(instance_short, 0, output) != 0 || strstr(output, "instance name:") == NULL) { (void)fprintf(stderr, "instance short %s\n", output); return 1; }
  if (run(instance_inspect, 1, output) != 125) return 1;
  if (chmod(root, 0755) != 0 || run(instance_inspect, 0, output) != 125 ||
      chmod(root, 0700) != 0) return 1;
  if (chmod(profiles, 0755) != 0 || run(instance_inspect, 0, output) != 125 ||
      chmod(profiles, 0700) != 0) return 1;
  {
    char identity[4096];
    if (snprintf(identity, sizeof(identity), "%s/%s.identity", root, instance_name) >= (int)sizeof(identity) ||
        chmod(identity, 0644) != 0 || run(instance_inspect, 0, output) != 125 ||
        chmod(identity, 0600) != 0 || chmod(digest_path, 0644) != 0 ||
        run(instance_inspect, 0, output) != 125 || chmod(digest_path, 0600) != 0) return 1;
  }
  if (run(no_root, 0, output) != 64) { (void)fprintf(stderr, "no-root\n"); return 1; }
  if (run(bad_root, 0, output) != 64) { (void)fprintf(stderr, "bad-root\n"); return 1; }
  if (run(explicit_path, 0, output) != 125) { (void)fprintf(stderr, "explicit\n"); return 1; }
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
        fputs("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$CT_INSTANCE_PROFILE_RUNTIME_LOG\"\ncase \"$CT_INSTANCE_PROFILE_RUNTIME_MODE\" in\n  valid) exec /bin/cat \"$CT_INSTANCE_PROFILE_RUNTIME_RECORD\" ;;\n  name) exec /bin/cat \"$CT_INSTANCE_PROFILE_RUNTIME_NAME_RECORD\" ;;\n  malformed) printf malformed ;;\n  oversized) /usr/bin/head -c 1048577 /dev/zero ;;\n  failure) exit 42 ;;\nesac\n", stream) == EOF ||
        fclose(stream) != 0 || chmod(runtime, 0700) != 0 || setenv("HOME", work, 1) != 0 ||
        setenv("PATH", fake, 1) != 0 || setenv("CT_INSTANCE_PROFILE_RUNTIME_LOG", runtime_log, 1) != 0 ||
        setenv("CT_INSTANCE_PROFILE_RUNTIME_RECORD", path, 1) != 0 ||
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
