#!/usr/bin/env bash
# Exercise archive refusal paths locally; acceptance requires the production static toolchain.
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
work=/tmp/mkchad-v1/container-tools-c11/package-archive-test
rm -rf -- "$work"
mkdir -p -- "$work/out"
commit=$(git -C "$root" rev-parse HEAD)
architecture=$(uname -m | tr '[:upper:]' '[:lower:]')

if bash "$root/scripts/build-package.sh" --build-root "$work/build" --version 1.0.0 \
  --source-commit 0000000000000000000000000000000000000000 --output-dir "$work/out" --libc glibc; then
  printf '%s\n' 'package build accepted a mismatched source commit' >&2
  exit 1
fi

archive_root="container-tools-1.0.0-${architecture}-glibc-${commit}"
mkdir -p -- "$work/malicious/$archive_root"
printf 'payload\n' > "$work/malicious/$archive_root/payload"
expect_invalid_archive() {
  local archive=$1 digest
  digest=$(sha256sum "$archive" | awk '{print $1}')
  if bash "$root/scripts/verify-package.sh" --archive "$archive" --sha256 "$digest" \
    --version 1.0.0 --source-commit "$commit" --architecture "$architecture" --libc glibc \
    --work-root "$work/invalid-verify"; then
    printf 'package verifier accepted malicious archive: %s\n' "$archive" >&2
    exit 1
  fi
  rm -rf -- "$work/invalid-verify"
}

self_authorized="$work/self-authorized/$archive_root"
while IFS= read -r path; do
  [[ $path == ./ ]] && { mkdir -p -- "$self_authorized"; continue; }
  if [[ $path == */ ]]; then
    mkdir -p -- "$self_authorized/${path%/}"
  else
    mkdir -p -- "$(dirname -- "$self_authorized/$path")"
    : > "$self_authorized/$path"
  fi
done < "$root/scripts/package-files.txt"
printf '{"schema":"container-tools.archive/v1","version":"1.0.0","architecture":"%s","libc":"glibc","source_commit":"%s"}\n' \
  "$architecture" "$commit" > "$self_authorized/archive.json"
printf '{"product_version":"1.0.0","source_commit":"%s","architecture":"%s"}\n' \
  "$commit" "$architecture" > "$self_authorized/share/container-tools/release.json"
printf '%s\n' unexpected > "$self_authorized/extra-file"
(cd "$work/self-authorized" && find "$archive_root" -print | LC_ALL=C sort > "$self_authorized/archive-files.txt")
tar -C "$work/self-authorized" -czf "$work/self-authorized.tar.gz" "$archive_root"
self_authorized_digest=$(sha256sum "$work/self-authorized.tar.gz" | awk '{print $1}')
if bash "$root/scripts/verify-package.sh" --archive "$work/self-authorized.tar.gz" \
  --sha256 "$self_authorized_digest" --version 1.0.0 --source-commit "$commit" \
  --architecture "$architecture" --libc glibc --work-root "$work/self-authorized-verify" \
  2>"$work/self-authorized.stderr"; then
  printf '%s\n' 'package verifier accepted a self-authorized extra file' >&2
  exit 1
fi
grep -Fq 'verify-package: archive has missing or extra entries' "$work/self-authorized.stderr" || {
  printf '%s\n' 'package verifier did not reject the self-authorized extra file at the layout boundary' >&2
  exit 1
}
tar -C "$work/malicious" -czf "$work/extra.tar.gz" "$archive_root"
expect_invalid_archive "$work/extra.tar.gz"
extra_digest=$(sha256sum "$work/extra.tar.gz" | awk '{print $1}')
set +e
bash "$root/scripts/verify-package.sh" --archive "$work/extra.tar.gz" --sha256 "$extra_digest" \
  --version ../escape --source-commit "$commit" --architecture "$architecture" --libc glibc \
  --work-root "$work/invalid-version"
invalid_version_status=$?
set -e
[[ $invalid_version_status == 64 ]] || {
  printf 'package verifier returned %s instead of usage for an unsafe version\n' "$invalid_version_status" >&2
  exit 1
}
ln -s payload "$work/malicious/$archive_root/link"
tar -C "$work/malicious" -czf "$work/link.tar.gz" "$archive_root"
rm -- "$work/malicious/$archive_root/link"
expect_invalid_archive "$work/link.tar.gz"
tar -C "$work/malicious" -cf "$work/duplicate.tar" "$archive_root"
tar -C "$work/malicious" -rf "$work/duplicate.tar" "$archive_root"
gzip -n "$work/duplicate.tar"
expect_invalid_archive "$work/duplicate.tar.gz"
tar -C "$work/malicious" --transform="s,^$archive_root,$archive_root/../escape," \
  -czf "$work/traversal.tar.gz" "$archive_root"
expect_invalid_archive "$work/traversal.tar.gz"
git clone --quiet --no-hardlinks "$root" "$work/dirty-source"
printf '%s\n' dirty > "$work/dirty-source/untracked"
if bash "$work/dirty-source/scripts/build-package.sh" --build-root "$work/dirty-build" --version 1.0.0 \
  --source-commit "$commit" --output-dir "$work/out" --libc musl; then
  printf '%s\n' 'package build accepted the dirty test checkout' >&2
  exit 1
fi

# A release candidate supplies a static archive through this explicit test input.
archive=${CT_PACKAGE_ARCHIVE:-}
digest=${CT_PACKAGE_SHA256:-}
if [[ -z $archive || -z $digest ]]; then
  printf '%s\n' 'package archive acceptance unavailable: no production static archive supplied' >&2
  exit 77
fi
bash "$root/scripts/verify-package.sh" --archive "$archive" --sha256 "$digest" \
  --version "${CT_PACKAGE_VERSION:?}" --source-commit "${CT_PACKAGE_SOURCE_COMMIT:?}" \
  --architecture "${CT_PACKAGE_ARCHITECTURE:-$architecture}" --libc "${CT_PACKAGE_LIBC:?}" \
  --work-root "$work/verify"
printf '%s\n' 'container-tools package archive tests passed'
