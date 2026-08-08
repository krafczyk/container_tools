/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_BACKEND_BUBBLEWRAP_H
#define CONTAINER_TOOLS_BACKEND_BUBBLEWRAP_H

#include "backend_nested.h"

/** Build one exact non-mutating Bubblewrap probe or launch argv. */
int ct_backend_bubblewrap_arguments(const struct ct_nested_request *request,
                                     int probe,
                                     struct ct_nested_command *command);
#endif
