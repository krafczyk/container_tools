/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "process.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
  char *const command[] = {"/bin/true", NULL};
  const char *const invalid[] = {"nan", "NaN", "inf", "-inf", "1x", "0", NULL};
  size_t index;

  for (index = 0U; invalid[index] != NULL; ++index) {
    if (setenv("CT_RUNTIME_OPERATION_TIMEOUT", invalid[index], 1) != 0 ||
        ct_process_run_operation(command, "probe") != 125) return 1;
  }
  if (setenv("CT_RUNTIME_OPERATION_TIMEOUT", "0.05", 1) != 0) return 1;
  {
    char *const ignoring[] = {"/bin/sh", "-c", "trap '' TERM; (trap '' TERM; while :; do sleep 1; done) & wait", NULL};
    if (ct_process_run_operation(ignoring, "probe") != 124) return 1;
  }
  if (unsetenv("CT_RUNTIME_OPERATION_TIMEOUT") != 0) return 1;
  return 0;
}
