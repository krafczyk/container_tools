/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "shebang.h"

#include <string.h>
#include <unistd.h>

int ct_shebang_parse(int descriptor, struct ct_shebang *shebang)
{
  char line[258];
  ssize_t length;
  char *start, *end, *argument;
  if (descriptor < 0 || shebang == NULL) return 1;
  memset(shebang, 0, sizeof(*shebang));
  length = pread(descriptor, line, sizeof(line) - 1U, 0);
  if (length < 3 || line[0] != '#' || line[1] != '!') return 1;
  line[length] = '\0'; end = strchr(line, '\n');
  if (end == NULL || end - line > 255) return 1;
  *end = '\0'; if (end > line && end[-1] == '\r') end[-1] = '\0';
  start = line + 2; while (*start == ' ' || *start == '\t') ++start;
  if (*start != '/') return 1;
  argument = start; while (*argument != '\0' && *argument != ' ' && *argument != '\t') ++argument;
  if (*argument != '\0') { *argument++ = '\0'; while (*argument == ' ' || *argument == '\t') ++argument; }
  if (strlen(start) >= sizeof(shebang->interpreter) || strlen(argument) >= sizeof(shebang->argument)) return 1;
  memcpy(shebang->interpreter, start, strlen(start) + 1U); memcpy(shebang->argument, argument, strlen(argument) + 1U);
  shebang->uses_env = strcmp(start, "/usr/bin/env") == 0 ? 1 : 0;
  return 0;
}
