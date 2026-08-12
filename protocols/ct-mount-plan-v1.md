# ct-mount-plan-v1

`ct-mount-plan-v1` is the immutable product-major-1 manifest grammar. The
production `ct_mount_plan_read_status` parser validates an exact bounded record
before selected-root execution or public read-only inspection reports consume
its metadata and ordered entries. Public reports never render the raw
NUL-delimited bytes.

## Report Path Encoding

The JSON inspection and comparison schemas require
`"path_encoding":"backslash-escaped-bytes"`. Their `caller_path` and
`target_path` values are ASCII encodings of the manifest path bytes: printable
ASCII bytes other than backslash and double quote are emitted unchanged;
backslash is `\\`, double quote is `\"`, and every other byte is `\xHH` using
lowercase hexadecimal. This representation is lossless for all valid Linux path
bytes and keeps the JSON document valid UTF-8. Human-readable reports apply the
same escaping so path bytes cannot introduce terminal controls or report lines.
