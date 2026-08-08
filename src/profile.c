/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "profile.h"

#include "sha256.h"

#include <string.h>

#define CT_PROFILE_MAX_FIELDS 2048U

int ct_profile_digest_fields(const char *const fields[], size_t field_count,
                             char digest[CT_PROFILE_DIGEST_HEX_LENGTH])
{
  struct ct_sha256 hash;
  unsigned char raw[32];
  size_t index;

  if (fields == NULL || digest == NULL || field_count == 0U ||
      field_count > CT_PROFILE_MAX_FIELDS) {
    return 1;
  }
  ct_sha256_init(&hash);
  for (index = 0U; index < field_count; ++index) {
    if (fields[index] == NULL) {
      return 1;
    }
    ct_sha256_update(&hash, fields[index], strlen(fields[index]) + 1U);
  }
  ct_sha256_final(&hash, raw);
  ct_sha256_hex(raw, digest);
  return 0;
}
