/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
  struct ct_runtime_config config = {{0}, {0}};
  char diagnostics[512];
  FILE *diagnostic_stream;
  char writable_root[] = "/tmp/mkchad-v1/container-tools-storage.XXXXXX";
  char adopted[512];
  char regular[512];
  char linked[512];
  FILE *stream;
  int saved_stderr;
  size_t diagnostic_size;
  if (ct_storage_ensure_directory("/tmp/container-tools-storage/..") == 0 ||
      ct_storage_ensure_directory("/tmp/container-tools-storage/.") == 0) {
    (void)fputs("terminal dot path component was accepted\n", stderr);
    return 1;
  }
  if (mkdtemp(writable_root) == NULL || chmod(writable_root, 0777) != 0 ||
      snprintf(adopted, sizeof(adopted), "%s/adopted", writable_root) >=
          (int)sizeof(adopted) ||
      snprintf(regular, sizeof(regular), "%s/regular", writable_root) >=
          (int)sizeof(regular) ||
      snprintf(linked, sizeof(linked), "%s/linked", writable_root) >=
          (int)sizeof(linked) || mkdir(adopted, 0777) != 0 ||
      setenv("CT_TEST_STORAGE_PRESENT_FOREIGN_UID", "1", 1) != 0 ||
      ct_storage_ensure_directory(adopted) != 0 ||
      unsetenv("CT_TEST_STORAGE_PRESENT_FOREIGN_UID") != 0 ||
      (stream = fopen(regular, "w")) == NULL || fclose(stream) != 0 ||
      ct_storage_ensure_directory(regular) == 0 ||
      symlink(adopted, linked) != 0 || ct_storage_ensure_directory(linked) == 0 ||
      unlink(linked) != 0 || unlink(regular) != 0 ||
      chmod(writable_root, 0700) != 0 || chmod(adopted, 0700) != 0 ||
      rmdir(adopted) != 0 || rmdir(writable_root) != 0) {
    (void)fputs("writable real directory was not adopted portably\n", stderr);
    return 1;
  }
  diagnostic_stream = tmpfile();
  saved_stderr = dup(STDERR_FILENO);
  if (diagnostic_stream == NULL || saved_stderr < 0 ||
      setenv("CT_RUNTIME_STORAGE_TIMEOUT", "zero", 1) != 0 ||
      dup2(fileno(diagnostic_stream), STDERR_FILENO) < 0 ||
      ct_storage_select_runtime("apptainer", &config) == 0 || fflush(stderr) != 0 ||
      fseek(diagnostic_stream, 0L, SEEK_SET) != 0 ||
      (diagnostic_size = fread(diagnostics, 1U, sizeof(diagnostics) - 1U,
                               diagnostic_stream)) == 0U ||
      dup2(saved_stderr, STDERR_FILENO) < 0 || close(saved_stderr) != 0 ||
      fclose(diagnostic_stream) != 0 || unsetenv("CT_RUNTIME_STORAGE_TIMEOUT") != 0) {
    (void)fputs("runtime storage timeout diagnostic setup failed\n", stderr);
    return 1;
  }
  diagnostics[diagnostic_size] = '\0';
  if (strstr(diagnostics,
             "container-tools: runtime storage: timeout: set CT_RUNTIME_STORAGE_TIMEOUT to a positive decimal no greater than 3600, then retry") == NULL) {
    (void)fputs("runtime storage diagnostic was not actionable\n", stderr);
    return 1;
  }
  return 0;
}
