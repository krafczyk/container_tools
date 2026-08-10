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
  const char nul_config[] = "CT_SINGULARITY_CACHE_DIR=/safe/cache\0UNKNOWN=/unsafe\n";
  int descriptor;
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
  if (descriptor < 0 || write(descriptor, nul_config, sizeof(nul_config) - 1U) !=
                            (ssize_t)(sizeof(nul_config) - 1U) ||
      close(descriptor) != 0 || ct_runtime_config_load(path, &config) == CT_CONFIG_OK) {
    (void)unlink(path);
    (void)fputs("embedded NUL runtime configuration was accepted\n", stderr);
    return 1;
  }
  (void)unlink(path);
  return 0;
}
