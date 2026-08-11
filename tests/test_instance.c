/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "instance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
  char root[] = "/tmp/mkchad-v1/container-tools-c11/native-instance.XXXXXX";
  char pending[4096];
  char image[4096];
  char mount_config[4096];
  char mount_plan_root[4096];
  char untouched[4096];
  char wrong_mode[4096];
  struct ct_state_pending record;
  struct stat status;
  int lock = -1;
  const char *name = "mkchad-0123456789abcdef0123456789abcdef";
  const char *profile = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const char *nonce = "0123456789abcdef0123456789abcdef";

  if (mkdtemp(root) == NULL || chmod(root, 0700) != 0 ||
      ct_state_pending_path(root, name, pending) != 0 ||
      ct_state_pending_write(pending, name, profile, nonce) != 0 ||
      stat(pending, &status) != 0 || (status.st_mode & 0777U) != 0600U ||
      ct_state_pending_read(pending, name, profile, &record) != 0 ||
      strcmp(record.name, name) != 0 || strcmp(record.profile, profile) != 0 ||
      strcmp(record.nonce, nonce) != 0 || ct_state_pending_clear(pending) != 0 ||
      access(pending, F_OK) == 0 ||
       snprintf(image, sizeof(image), "%s/image.sif", root) >= (int)sizeof(image) ||
       snprintf(mount_config, sizeof(mount_config), "%s/missing-mount.conf", root) >= (int)sizeof(mount_config) ||
       snprintf(mount_plan_root, sizeof(mount_plan_root), "%s/mount-plans", root) >= (int)sizeof(mount_plan_root) ||
       snprintf(untouched, sizeof(untouched), "%s/identity-root", root) >= (int)sizeof(untouched) ||
       snprintf(wrong_mode, sizeof(wrong_mode), "%s/wrong-mode", root) >=
           (int)sizeof(wrong_mode) ||
       mkdir(wrong_mode, 0750) != 0 || chmod(wrong_mode, 0750) != 0 ||
       ct_state_prepare_root(wrong_mode) == 0 || stat(wrong_mode, &status) != 0 ||
       (status.st_mode & 0777U) != 0750U) {
    return 1;
  }
  {
    FILE *stream = fopen(pending, "w");
    pid_t child;
    int child_status;
    if (stream == NULL || fputs("ct-instance-profile-v4\n", stream) == EOF || fclose(stream) != 0 ||
        chmod(pending, 0600) != 0 || ct_state_pending_read(pending, name, profile, &record) == 0 ||
        ct_state_lock(root, name, &lock) != 0 || (child = fork()) < 0) return 1;
    if (child == 0) {
      int child_lock = -1;
      (void)setenv("CT_INSTANCE_LOCK_TIMEOUT", "0.01", 1);
      _exit(ct_state_lock(root, name, &child_lock) == 0 ? 1 : 0);
    }
    if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
      ct_state_unlock(lock);
      return 1;
    }
    ct_state_unlock(lock);
    if (unlink(pending) != 0) return 1;
  }
  {
    FILE *stream = fopen(image, "w");
    FILE *diagnostic_stream;
    char *identity[] = {"--apptainer", "--ct-instance-root", untouched, "--", image, "/bin/true"};
    char *reserved[] = {"--apptainer", "--ct-instance-root", untouched, "--ct-env",
                        "SINGULARITYENV_CONTAINER_TOOLS_PROFILE=caller", "--", image, "/bin/true"};
    char diagnostics[512];
    size_t diagnostic_size;
    int saved_stderr;
    if (stream == NULL || fputs("image\n", stream) == EOF || fclose(stream) != 0 ||
        setenv("HOME", root, 1) != 0 || setenv("CT_MOUNT_CFG", mount_config, 1) != 0 ||
        setenv("CT_MOUNT_PLAN_STATE_ROOT", mount_plan_root, 1) != 0 ||
        setenv("CT_DRY_RUN", "1", 1) != 0 ||
        ct_instance_command(6, identity, 1) != 0 || access(untouched, F_OK) == 0 ||
        (diagnostic_stream = tmpfile()) == NULL || (saved_stderr = dup(STDERR_FILENO)) < 0 ||
        fflush(stderr) == EOF || dup2(fileno(diagnostic_stream), STDERR_FILENO) < 0 ||
        ct_instance_command(8, reserved, 1) == 0 || fflush(stderr) == EOF ||
        dup2(saved_stderr, STDERR_FILENO) < 0 || close(saved_stderr) != 0 ||
        fseek(diagnostic_stream, 0L, SEEK_SET) != 0 ||
        (diagnostic_size = fread(diagnostics, 1U, sizeof(diagnostics) - 1U,
                                 diagnostic_stream)) == 0U ||
        fclose(diagnostic_stream) != 0 || unsetenv("CT_DRY_RUN") != 0) return 1;
    diagnostics[diagnostic_size] = '\0';
    if (strstr(diagnostics,
               "container-tools: persistent instance request: usage: use 'container-tools instance exec --help' for valid syntax") == NULL) {
      return 1;
    }
  }
  return 0;
}
