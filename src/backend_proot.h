/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_BACKEND_PROOT_H
#define CONTAINER_TOOLS_BACKEND_PROOT_H
#include "backend_nested.h"
/** Build one minimal PRoot probe or launch argv, including explicit `/proc`. */
int ct_backend_proot_arguments(const struct ct_nested_request *request,
                               int probe, struct ct_nested_command *command);
#endif
