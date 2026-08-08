/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_SHEBANG_H
#define CONTAINER_TOOLS_SHEBANG_H

/** One bounded Linux shebang interpreter record. */
struct ct_shebang { char interpreter[4096]; char argument[256]; int uses_env; };
/**
 * Parse the first line of a script descriptor as a bounded Linux shebang.
 *
 * @param descriptor Open script descriptor.
 * @param shebang Destination interpreter description.
 * @return Zero for a bounded absolute interpreter or `/usr/bin/env` form,
 *         otherwise nonzero. The descriptor remains open and its offset is
 *         unchanged; recursive resolution belongs to `ct_executable_resolve`.
 */
int ct_shebang_parse(int descriptor, struct ct_shebang *shebang);

#endif
