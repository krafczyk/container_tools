/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "buildx.h"

/** Invoke the test-seam Buildx path without installing a second product executable. */
int main(int argument_count, char **arguments)
{
  return ct_buildx_exec_command(argument_count - 1, arguments + 1);
}
