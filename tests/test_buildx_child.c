/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int main(int argument_count, char **arguments)
{
  char destination[4096] = "";
  int index;
  for (index = 0; index < argument_count; ++index) {
    if (strcmp(arguments[index], "--cache-to") == 0 && index + 1 < argument_count) {
      const char *prefix = "type=local,dest=";
      if (strncmp(arguments[index + 1], prefix, strlen(prefix)) == 0) {
        const char *value = arguments[index + 1] + strlen(prefix);
        const size_t length = strcspn(value, ",");
        if (length >= sizeof(destination)) return 125;
        memcpy(destination, value, length);
        destination[length] = '\0';
      }
    }
  }
  if (getenv("CT_TEST_BUILDX_RAN") != NULL) {
    FILE *marker = fopen(getenv("CT_TEST_BUILDX_RAN"), "w");
    if (marker == NULL) return 125;
    (void)fclose(marker);
  }
  if (getenv("CT_TEST_BUILDX_SIGNAL") != NULL) raise(SIGTERM);
  if (destination[0] != '\0' && getenv("CT_TEST_BUILDX_NO_INDEX") == NULL) {
    char index_path[4096];
    FILE *stream;
    if (snprintf(index_path, sizeof(index_path), "%s/index.json", destination) >= (int)sizeof(index_path)) return 125;
    stream = fopen(index_path, "w");
    if (stream == NULL) return errno == ENOENT ? 125 : 126;
    (void)fputs("{}\n", stream);
    (void)fclose(stream);
  }
  if (getenv("CT_TEST_BUILDX_SLEEP") != NULL) {
    const struct timespec delay = {1, 0};
    (void)nanosleep(&delay, NULL);
  }
  return getenv("CT_TEST_BUILDX_EXIT") == NULL ? 0 : atoi(getenv("CT_TEST_BUILDX_EXIT"));
}
