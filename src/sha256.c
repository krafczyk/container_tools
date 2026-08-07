/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "sha256.h"

#include <string.h>

static const uint32_t ct_sha256_rounds[64] = {
  0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
  0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
  0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
  0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
  0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
  0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
  0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
  0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t ct_rotate_right(uint32_t value, unsigned int count)
{
  return (value >> count) | (value << (32U - count));
}

static void ct_sha256_block(struct ct_sha256 *context, const unsigned char block[64])
{
  uint32_t words[64];
  uint32_t a, b, c, d, e, f, g, h;
  size_t index;

  for (index = 0U; index < 16U; ++index) {
    words[index] = ((uint32_t)block[index * 4U] << 24U) | ((uint32_t)block[index * 4U + 1U] << 16U) |
                   ((uint32_t)block[index * 4U + 2U] << 8U) | (uint32_t)block[index * 4U + 3U];
  }
  for (index = 16U; index < 64U; ++index) {
    const uint32_t s0 = ct_rotate_right(words[index - 15U], 7U) ^ ct_rotate_right(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
    const uint32_t s1 = ct_rotate_right(words[index - 2U], 17U) ^ ct_rotate_right(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
    words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
  }
  a = context->state[0]; b = context->state[1]; c = context->state[2]; d = context->state[3];
  e = context->state[4]; f = context->state[5]; g = context->state[6]; h = context->state[7];
  for (index = 0U; index < 64U; ++index) {
    const uint32_t s1 = ct_rotate_right(e, 6U) ^ ct_rotate_right(e, 11U) ^ ct_rotate_right(e, 25U);
    const uint32_t choose = (e & f) ^ ((~e) & g);
    const uint32_t temporary_one = h + s1 + choose + ct_sha256_rounds[index] + words[index];
    const uint32_t s0 = ct_rotate_right(a, 2U) ^ ct_rotate_right(a, 13U) ^ ct_rotate_right(a, 22U);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temporary_two = s0 + majority;
    h = g; g = f; f = e; e = d + temporary_one; d = c; c = b; b = a; a = temporary_one + temporary_two;
  }
  context->state[0] += a; context->state[1] += b; context->state[2] += c; context->state[3] += d;
  context->state[4] += e; context->state[5] += f; context->state[6] += g; context->state[7] += h;
}

void ct_sha256_init(struct ct_sha256 *context)
{
  static const uint32_t initial[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  memcpy(context->state, initial, sizeof(initial));
  context->length = 0U;
  context->used = 0U;
}

void ct_sha256_update(struct ct_sha256 *context, const void *data, size_t length)
{
  const unsigned char *input = data;
  while (length > 0U) {
    const size_t available = 64U - context->used;
    const size_t copied = length < available ? length : available;
    memcpy(context->block + context->used, input, copied);
    context->used += copied; input += copied; length -= copied;
    if (context->used == 64U) { ct_sha256_block(context, context->block); context->length += 512U; context->used = 0U; }
  }
}

void ct_sha256_final(struct ct_sha256 *context, unsigned char result[32])
{
  uint64_t bit_length = context->length + (uint64_t)context->used * 8U;
  size_t index;
  context->block[context->used++] = 0x80U;
  if (context->used > 56U) { memset(context->block + context->used, 0, 64U - context->used); ct_sha256_block(context, context->block); context->used = 0U; }
  memset(context->block + context->used, 0, 56U - context->used);
  for (index = 0U; index < 8U; ++index) context->block[63U - index] = (unsigned char)(bit_length >> (index * 8U));
  ct_sha256_block(context, context->block);
  for (index = 0U; index < 8U; ++index) {
    result[index * 4U] = (unsigned char)(context->state[index] >> 24U);
    result[index * 4U + 1U] = (unsigned char)(context->state[index] >> 16U);
    result[index * 4U + 2U] = (unsigned char)(context->state[index] >> 8U);
    result[index * 4U + 3U] = (unsigned char)context->state[index];
  }
}

void ct_sha256_hex(const unsigned char digest[32], char output[65])
{
  static const char digits[] = "0123456789abcdef";
  size_t index;
  for (index = 0U; index < 32U; ++index) { output[index * 2U] = digits[digest[index] >> 4U]; output[index * 2U + 1U] = digits[digest[index] & 15U]; }
  output[64] = '\0';
}
