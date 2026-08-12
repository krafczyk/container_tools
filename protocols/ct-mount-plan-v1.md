# ct-mount-plan-v1

`ct-mount-plan-v1` is the immutable product-major-1 manifest grammar. The
production `ct_mount_plan_read_status` parser validates an exact bounded record
before selected-root execution or public read-only inspection reports consume
its metadata and ordered entries. Public reports never render the raw
NUL-delimited bytes.

## Managed Cache Clear

`container-tools mount plan clear [--help]` is the sole host-side cache cleanup
grammar. It selects the producer state root, including the narrow
`CT_MOUNT_PLAN_STATE_ROOT` test override, and accepts no deletion path. A clear
requires a current-user-owned mode-0700 root and protocol directories and
current-user-owned mode-0600 regular entries. It deletes only digest-named
manifests, per-digest lock files, and digest-named temporary files after locking
the cache-wide `.locks/.cache.lock` exclusively; publishers hold that lock shared.
Any symlink, unexpected entry, ownership or mode failure, entry overflow, or
timeout fails closed with exit 125. The private root, `.locks`, and cache-wide
lock remain as synchronization metadata after managed entries are cleared.

## Report Path Encoding

The JSON inspection and comparison schemas require
`"path_encoding":"backslash-escaped-bytes"`. Their `caller_path` and
`target_path` values are ASCII encodings of the manifest path bytes: printable
ASCII bytes other than backslash and double quote are emitted unchanged;
backslash is `\\`, double quote is `\"`, and every other byte is `\xHH` using
lowercase hexadecimal. This representation is lossless for all valid Linux path
bytes and keeps the JSON document valid UTF-8. Human-readable reports apply the
same escaping so path bytes cannot introduce terminal controls or report lines.
