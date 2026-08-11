/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
  struct ct_runtime_config config;
  char path[] = "test-config-nul-XXXXXX";
  char diagnostics[512];
  const char nul_config[] = "CT_SINGULARITY_CACHE_DIR=/safe/cache\0UNKNOWN=/unsafe\n";
  int descriptor;
  int saved_stderr;
  FILE *diagnostic_stream;
  size_t diagnostic_size;
  const char *valid =
      "# comment\n"
      "CT_SINGULARITY_CACHE_DIR=/safe/cache with spaces\n"
      "CT_SINGULARITY_TMP_DIR=/safe/tmp\n";

  if (ct_runtime_config_parse(valid, &config) != CT_CONFIG_OK ||
      strcmp(config.singularity_cache_dir, "/safe/cache with spaces") != 0 ||
      strcmp(config.singularity_tmp_dir, "/safe/tmp") != 0) {
    (void)fputs("valid runtime configuration was rejected\n", stderr);
    return 1;
  }
  const char removed_key[] = {'C', 'T', '_', 'D', 'O', 'C', 'K', 'E', 'R', '_',
                              'B', 'U', 'I', 'L', 'D', '_', 'C', 'A', 'C', 'H',
                              'E', '_', 'D', 'I', 'R', '=', '/', 't', 'm', 'p',
                              '/', 'c', 'a', 'c', 'h', 'e', '\n', '\0'};
  if (ct_runtime_config_parse("UNKNOWN=/tmp\n", &config) == CT_CONFIG_OK ||
      ct_runtime_config_parse("CT_SINGULARITY_CACHE_DIR=relative\n", &config) == CT_CONFIG_OK ||
      ct_runtime_config_parse(removed_key, &config) == CT_CONFIG_OK ||
      ct_runtime_config_parse("CT_SINGULARITY_CACHE_DIR=/a\nCT_SINGULARITY_CACHE_DIR=/b\n",
                              &config) == CT_CONFIG_OK) {
    (void)fputs("malformed runtime configuration was accepted\n", stderr);
    return 1;
  }
  descriptor = mkstemp(path);
  diagnostic_stream = tmpfile();
  saved_stderr = dup(STDERR_FILENO);
  if (descriptor < 0 || write(descriptor, nul_config, sizeof(nul_config) - 1U) !=
                             (ssize_t)(sizeof(nul_config) - 1U) ||
      close(descriptor) != 0 || diagnostic_stream == NULL || saved_stderr < 0 ||
      dup2(fileno(diagnostic_stream), STDERR_FILENO) < 0 ||
      ct_runtime_config_load(path, &config) == CT_CONFIG_OK || fflush(stderr) != 0 ||
      fseek(diagnostic_stream, 0L, SEEK_SET) != 0 ||
      (diagnostic_size = fread(diagnostics, 1U, sizeof(diagnostics) - 1U,
                               diagnostic_stream)) == 0U ||
      dup2(saved_stderr, STDERR_FILENO) < 0 || close(saved_stderr) != 0 ||
      fclose(diagnostic_stream) != 0 || unlink(path) != 0) {
    (void)unlink(path);
    (void)fputs("embedded NUL runtime configuration was accepted\n", stderr);
    return 1;
  }
  diagnostics[diagnostic_size] = '\0';
  if (strstr(diagnostics,
             "container-tools: runtime configuration: configuration: set CT_RUNTIME_CFG to an owned regular file containing only supported absolute-path variables, then retry") == NULL) {
    (void)fputs("runtime configuration diagnostic was not actionable\n", stderr);
    return 1;
  }
  return 0;
}
