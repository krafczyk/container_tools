/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef CONTAINER_TOOLS_PROFILE_H
#define CONTAINER_TOOLS_PROFILE_H

#include <stddef.h>

/** Storage required for one lowercase SHA-256 profile digest and its NUL. */
#define CT_PROFILE_DIGEST_HEX_LENGTH 65U
/** Maximum ordered field count accepted by ct_profile_digest_fields. */
#define CT_PROFILE_MAX_FIELDS 16384U

/**
 * Hash one complete ordered profile field sequence using the product's
 * NUL-delimited SHA-256 representation.
 *
 * @param fields Finalized profile fields; none may be null or contain NUL
 *        outside the normal C string terminator.
 * @param field_count Number of fields, from one through CT_PROFILE_MAX_FIELDS.
 * @param digest Destination for a lowercase 64-hex digest plus NUL.
 * @return Zero on success, otherwise nonzero without producing an identity.
 */
int ct_profile_digest_fields(const char *const fields[], size_t field_count,
                             char digest[CT_PROFILE_DIGEST_HEX_LENGTH]);

#endif
