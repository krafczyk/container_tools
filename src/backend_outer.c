/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_outer.h"

#include "process.h"

#include <stdio.h>
#include <string.h>

int ct_backend_outer_dispatch(char *const arguments[], size_t count)
{
  if (arguments == NULL || count == 0U || arguments[0] == NULL || arguments[count] != NULL) return 125;
  return ct_process_exec(arguments, NULL, 0U);
}

int ct_backend_outer_projection_mount(const char *backend, const char *source,
                                      const char *target, char *output,
                                      size_t output_size, const char **flag)
{
  int written;

  if (backend == NULL || source == NULL || target == NULL || output == NULL ||
      output_size == 0U || flag == NULL) return 1;
  *flag = "--mount";
  if (strcmp(backend, "docker") == 0) {
    written = snprintf(output, output_size,
                       "type=bind,source=%s,target=%s,bind-recursive=disabled",
                       source, target);
  } else if (strcmp(backend, "podman") == 0) {
    written = snprintf(output, output_size,
                       "type=bind,source=%s,target=%s,bind-nonrecursive",
                       source, target);
  } else if (strcmp(backend, "singularity") == 0 ||
             strcmp(backend, "apptainer") == 0) {
    written = snprintf(output, output_size, "type=bind,src=%s,dst=%s", source,
                       target);
  } else {
    return 1;
  }
  return written < 0 || (size_t)written >= output_size;
}
