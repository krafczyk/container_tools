/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_TRAMPOLINE_H
#define CONTAINER_TOOLS_TRAMPOLINE_H
/**
 * Admit descriptor transport and directly execute its supplied target argv.
 *
 * The internal-only grammar accepts an optional non-CLOEXEC `--descriptor-fd N`.
 * `--probe CWD TARGET DEVICE INODE TYPE` verifies the selected-root cwd and one
 * parent-admitted projection identity before repeating both checks through a
 * descendant self-exec. A target argv is executed only after descriptor admission.
 */
int ct_trampoline_main(int argument_count, char **arguments, const char *build_identity);
#endif
