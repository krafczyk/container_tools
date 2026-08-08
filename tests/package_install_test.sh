#!/usr/bin/env bash
# Exercise the prefix installer against a supplied production static archive.
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
work=/tmp/mkchad-v1/container-tools-c11/package-install-test
rm -rf -- "$work"
mkdir -p -- "$work"
archive=${CT_PACKAGE_ARCHIVE:-}
digest=${CT_PACKAGE_SHA256:-}
if [[ -z $archive || -z $digest ]]; then
  printf '%s\n' 'package install acceptance unavailable: no production static archive supplied' >&2
  exit 77
fi
args=(--archive "$archive" --sha256 "$digest" --version "${CT_PACKAGE_VERSION:?}" \
  --source-commit "${CT_PACKAGE_SOURCE_COMMIT:?}" --architecture "${CT_PACKAGE_ARCHITECTURE:?}" \
  --libc "${CT_PACKAGE_LIBC:?}")
installer=(bash "$root/scripts/install-package.sh")
"${installer[@]}" --check "${args[@]}" --prefix "$work/check-prefix"
"${installer[@]}" --apply "${args[@]}" --prefix "$work/prefix-a" --recovery-dir "$work/recovery-a"
"${installer[@]}" --verify "${args[@]}" --prefix "$work/prefix-a"
[[ -f $work/prefix-a/share/container-tools/release.json ]] || {
  printf '%s\n' 'installer exposed a broken package share link' >&2
  exit 1
}
printf '%s\n' unrelated > "$work/prefix-a/bin/unrelated-tool"
printf '%s\n' unrelated > "$work/prefix-a/share/unrelated-data"
"${installer[@]}" --apply "${args[@]}" --prefix "$work/prefix-a" --recovery-dir "$work/recovery-a"
[[ -f $work/prefix-a/bin/unrelated-tool && -f $work/prefix-a/share/unrelated-data ]] || {
  printf '%s\n' 'installer replaced unrelated prefix content' >&2
  exit 1
}
"${installer[@]}" --apply "${args[@]}" --prefix "$work/prefix-b" --recovery-dir "$work/recovery-b"
mv "$work/prefix-b" "$work/moved-prefix"
"${installer[@]}" --verify "${args[@]}" --prefix "$work/moved-prefix"
if CT_PACKAGE_INSTALL_INTERRUPT_AT=after-stage "${installer[@]}" --apply "${args[@]}" \
  --prefix "$work/interrupted" --recovery-dir "$work/recovery-interrupted"; then
  printf '%s\n' 'installer did not interrupt its owned stage boundary' >&2
  exit 1
fi
"${installer[@]}" --apply "${args[@]}" --prefix "$work/interrupted" --recovery-dir "$work/recovery-interrupted"
for boundary in after-links after-expose; do
  if CT_PACKAGE_INSTALL_INTERRUPT_AT=$boundary "${installer[@]}" --apply "${args[@]}" \
    --prefix "$work/interrupted-$boundary" --recovery-dir "$work/recovery-$boundary"; then
    printf 'installer did not interrupt its owned %s boundary\n' "$boundary" >&2
    exit 1
  fi
  "${installer[@]}" --apply "${args[@]}" --prefix "$work/interrupted-$boundary" \
    --recovery-dir "$work/recovery-$boundary"
  "${installer[@]}" --verify "${args[@]}" --prefix "$work/interrupted-$boundary"
done
if CT_PACKAGE_INSTALL_INTERRUPT_AT=after-stage "${installer[@]}" --apply "${args[@]}" \
  --prefix "$work/corrupt-recovery" --recovery-dir "$work/recovery-corrupt"; then
  printf '%s\n' 'installer did not create the corrupt-recovery fixture' >&2
  exit 1
fi
candidate=$(find "$work/recovery-corrupt" -name candidate.tar.gz -print -quit)
printf 'changed\n' >> "$candidate"
if "${installer[@]}" --apply "${args[@]}" --prefix "$work/corrupt-recovery" \
  --recovery-dir "$work/recovery-corrupt"; then
  printf '%s\n' 'installer resumed recovery state for different archive bytes' >&2
  exit 1
fi
mkdir -p -- "$work/unmanaged/bin" "$work/unmanaged/share"
printf '%s\n' unmanaged > "$work/unmanaged/bin/container-tools"
if "${installer[@]}" --apply "${args[@]}" --prefix "$work/unmanaged" \
  --recovery-dir "$work/recovery-unmanaged"; then
  printf '%s\n' 'installer replaced an unmanaged prefix' >&2
  exit 1
fi
cp -a -- "$work/prefix-a" "$work/mixed-prefix"
printf '\n# mismatched component\n' >> "$work/mixed-prefix/.container-tools/current/bin/ct_args.sh"
if "${installer[@]}" --verify "${args[@]}" --prefix "$work/mixed-prefix"; then
  printf '%s\n' 'installer accepted a cross-file package mismatch' >&2
  exit 1
fi
if "${installer[@]}" --verify "${args[@]}" --prefix "$work/not-installed"; then
  printf '%s\n' 'installer accepted an incomplete prefix' >&2
  exit 1
fi
printf '%s\n' 'container-tools package install tests passed'
