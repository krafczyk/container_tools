/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
  struct ct_runtime_config config = {{0}, {0}};
  char diagnostics[512];
  FILE *diagnostic_stream;
  int saved_stderr;
  size_t diagnostic_size;
  if (ct_storage_ensure_private_directory("/tmp/container-tools-storage/..") == 0 ||
      ct_storage_ensure_private_directory("/tmp/container-tools-storage/.") == 0) {
    (void)fputs("terminal dot path component was accepted\n", stderr);
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
