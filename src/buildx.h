/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_BUILDX_H
#define CONTAINER_TOOLS_BUILDX_H

#include "config.h"

#include <stdbool.h>
#include <stddef.h>

#define CT_BUILDX_PATH_MAX 4096U

/** Lock-held architecture-specific Buildx cache generation. */
struct ct_buildx_transaction {
  int lock_descriptor;
  char namespace_path[CT_BUILDX_PATH_MAX];
  char current_path[CT_BUILDX_PATH_MAX];
  char staging_path[CT_BUILDX_PATH_MAX];
  bool has_current;
};

/** Validate the required Docker Buildx child prefix and reject transaction flags. */
bool ct_buildx_child_is_valid(const char *const *arguments, size_t argument_count);

/** Prepare the lock-held generation around one architecture. */
int ct_buildx_prepare(struct ct_buildx_transaction *transaction,
                      const struct ct_runtime_config *config,
                      const char *architecture);

/** Promote a successful local-cache export, retaining the prior generation on failure. */
int ct_buildx_commit(struct ct_buildx_transaction *transaction);

/** Discard an incomplete export and release the architecture lock. */
int ct_buildx_discard(struct ct_buildx_transaction *transaction);

/** Execute the public `buildx exec` operation with its transaction in scope. */
int ct_buildx_exec_command(int argument_count, char *const arguments[]);

#endif
