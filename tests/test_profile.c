/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "profile.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
  const char *first[] = {"apptainer", "1000", "image", "mount"};
  const char *changed[] = {"apptainer", "1000", "image", "other-mount"};
  char first_digest[CT_PROFILE_DIGEST_HEX_LENGTH];
  char second_digest[CT_PROFILE_DIGEST_HEX_LENGTH];

  if (ct_profile_digest_fields(first, sizeof(first) / sizeof(first[0]), first_digest) != 0 ||
      ct_profile_digest_fields(first, sizeof(first) / sizeof(first[0]), second_digest) != 0 ||
      strcmp(first_digest, second_digest) != 0 || strlen(first_digest) != 64U ||
      ct_profile_digest_fields(changed, sizeof(changed) / sizeof(changed[0]), second_digest) != 0 ||
      strcmp(first_digest, second_digest) == 0 ||
      ct_profile_digest_fields(first, 0U, second_digest) == 0) {
    return 1;
  }
  return 0;
}
