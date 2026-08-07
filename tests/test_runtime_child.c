/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argument_count, char **arguments)
{
  FILE *output = fopen(getenv("CT_TEST_OUTPUT"), "w");
  const char *cache = getenv("APPTAINER_CACHEDIR");
  const char *temporary = getenv("APPTAINER_TMPDIR");
  int index;
  if (output == NULL) return 125;
  if (getenv("CT_TEST_SLEEP") != NULL) {
    const struct timespec delay = {2, 0};
    (void)nanosleep(&delay, NULL);
  }
  (void)fprintf(output, "cache=%s\ntmp=%s\nargc=%d\n", cache == NULL ? "" : cache,
                temporary == NULL ? "" : temporary, argument_count);
  for (index = 0; index < argument_count; ++index) (void)fprintf(output, "arg=%s\n", arguments[index]);
  (void)fclose(output);
  return getenv("CT_TEST_EXIT") == NULL ? 0 : atoi(getenv("CT_TEST_EXIT"));
}
