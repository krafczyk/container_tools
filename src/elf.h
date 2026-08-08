/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_ELF_H
#define CONTAINER_TOOLS_ELF_H

#include <elf.h>

/** Bounded ELF entry-point classification. */
enum ct_elf_kind { CT_ELF_INVALID = 0, CT_ELF_STATIC = 1, CT_ELF_DYNAMIC = 2 };
/** ELF facts needed by selected-root planning. */
struct ct_elf_info { enum ct_elf_kind kind; unsigned int machine; char interpreter[4096]; };

/**
 * Inspect a regular ELF file descriptor without changing its offset.
 *
 * @param descriptor Open executable descriptor.
 * @param info Destination ELF facts.
 * @return Zero for a stable supported native ELF, otherwise nonzero. The
 *         descriptor remains open and its offset is unchanged.
 */
int ct_elf_inspect(int descriptor, struct ct_elf_info *info);
/** Return nonzero when `machine` matches the current Linux build target. */
int ct_elf_machine_is_native(unsigned int machine);

#endif
