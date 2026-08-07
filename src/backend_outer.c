/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "backend_outer.h"

#include "process.h"

int ct_backend_outer_dispatch(char *const arguments[], size_t count)
{
  if (arguments == NULL || count == 0U || arguments[0] == NULL || arguments[count] != NULL) return 125;
  return ct_process_run(arguments, NULL, 0U);
}
