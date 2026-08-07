/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_SHA256_H
#define CONTAINER_TOOLS_SHA256_H

#include <stddef.h>
#include <stdint.h>

/** Incremental SHA-256 state for closed container-tools protocol records. */
struct ct_sha256 {
  uint32_t state[8];
  uint64_t length;
  unsigned char block[64];
  size_t used;
};

/** Initialize a SHA-256 digest calculation. */
void ct_sha256_init(struct ct_sha256 *context);
/** Add bytes to an initialized SHA-256 digest calculation. */
void ct_sha256_update(struct ct_sha256 *context, const void *data, size_t length);
/** Finish a digest calculation into a 32-byte result. */
void ct_sha256_final(struct ct_sha256 *context, unsigned char result[32]);
/** Encode a 32-byte digest as lowercase hexadecimal plus a terminating NUL. */
void ct_sha256_hex(const unsigned char digest[32], char output[65]);

#endif
