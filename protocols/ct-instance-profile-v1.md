# ct-instance-profile-v1

`ct-instance-profile-v1` is the bounded, canonical NUL-delimited persistent
instance profile record. It is an explainable read-only representation of the
identity inputs that select a managed native instance. It excludes payload argv,
per-call `--ct-env`, creation nonces, timestamps, and credentials.

Every field, including the final source-identity field, ends in NUL. The record
is limited to 1 MiB, at most 1024 ordered supplementary groups, and at most
4096 ordered finalized mounts. Its field sequence is:

```text
version, record_digest, profile_digest, instance_name, backend,
profile_grammar, runtime_argument_digest, uid, gid, hostname, home,
instance_root, image_path, image_identity, bootstrap_path, bootstrap_identity,
projection_grammar, projection_digest, group_mode, group_count,
group_count * group, mount_plan_digest, mount_count,
mount_count * (flag, descriptor, role, access, recursion, generated, semantic,
               resolved_source, source_identity)
```

`record_digest` is the lowercase SHA-256 digest over the same sequence with
that field omitted. `profile_digest` remains independent metadata: it is the
full existing profile identity, and `instance_name` must be exactly
`mkchad-` plus its first 32 lowercase hexadecimal characters. The parser
rejects malformed, future-major, oversized, internally tampered, or semantically
inconsistent records.

The profile grammar is currently `ct-instance-profile-v4`; the record grammar
does not redefine the lifecycle identity. `runtime_argument_digest` is either
`absent` or a lowercase SHA-256 digest, never its raw argument. Bootstrap path
and identity are both `absent`, or are both populated. Mount `source_identity`
is `absent` only when it has no profile effect. Paths in manifests remain raw
Linux bytes; reports use the documented lossless escaped-byte representation.

## Inspection

`container-tools instance profile inspect [--json] [--ct-instance-root ROOT]
[--] [PATH|DIGEST]` reads one existing record without creating the root. No
argument reads `/.container-tools-instance-profile`. An explicit path is always
a path, including `./<hex-prefix>`. A bare nonempty lowercase hexadecimal prefix
up to 64 characters requires `--ct-instance-root ROOT` and selects only when
exactly one `ROOT/profiles/DIGEST.manifest` matches; a full digest retains exact
selection. Zero matches are ordinary selection failures and multiple matches
report ambiguity without revealing private paths. The root is normalized with
the private absolute instance-root scalar rules and must not contain colon,
comma, or newline. Digest lookup verifies the filename selector, the record
`profile_digest`, and the internal `record_digest`.

Human output and the closed JSON schema
`container-tools.instance-profile-inspect/v1` use
`"path_encoding":"backslash-escaped-bytes"`: printable ASCII remains literal
except backslash and quote; those become `\\` and `\"`; all other bytes become
lowercase `\xHH`. Valid inspection exits 0, command misuse exits 64, and read,
validation, or configuration failures exit 125 without JSON. Output-destination
failures also exit 125 but may leave bytes already accepted by that destination.

`container-tools instance inspect [--json] (--ct-instance-root ROOT |
--apptainer | --singularity) NAME_OR_HASH` is the lifecycle lookup command.
`NAME_OR_HASH` is `mkchad-` plus a nonempty lowercase hexadecimal prefix up to
32 characters, or the bare prefix. Root mode uniquely resolves shortened input
among strict private `ROOT/NAME.identity` indexes, then reads the exact private
`ROOT/profiles/DIGEST.manifest`; it does not access a backend. Backend mode
lists managed runtime names only to uniquely resolve shortened input, then
executes against the exact normalized `instance://NAME` and parses the bounded
bytes from `/.container-tools-instance-profile`. Exact 32-hex selectors bypass
enumeration in both modes. Zero matches fail selection and multiple matches
report ambiguity without exposing private paths. Both forms require the profile
digest prefix to agree with the resolved managed name and return the identical
report schema.

Publication occurs after the existing full profile digest has been finalized.
`ROOT/profiles` and its manifests are current-user-owned mode `0700` and `0600`
regular private state. A final manifest is atomically created without replacing
an existing record; concurrent byte-identical publishers reuse it, while any
unsafe, malformed, unstable, or conflicting record fails closed. The internal
profile bind is nonsemantic and excluded from the mount plan and profile hash.
