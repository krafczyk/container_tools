#!/usr/bin/env bash
# Verify one closed container-tools archive without selecting or installing it.
set -euo pipefail
umask 077

usage() {
  printf '%s\n' 'usage: verify-package.sh --archive FILE --sha256 64_HEX --version VERSION --source-commit 40_HEX --architecture ARCH --libc musl|glibc [--work-root ABSOLUTE_DIR]' >&2
  exit 64
}

archive=''
digest=''
version=''
source_commit=''
architecture=''
libc=''
work_root=/tmp/mkchad-v1/container-tools-c11/package-verify
while (($#)); do
  case $1 in
    --archive|--sha256|--version|--source-commit|--architecture|--libc|--work-root)
      (($# >= 2)) || usage
      case $1 in
        --archive) archive=$2 ;; --sha256) digest=$2 ;; --version) version=$2 ;;
        --source-commit) source_commit=$2 ;; --architecture) architecture=$2 ;;
        --libc) libc=$2 ;; --work-root) work_root=$2 ;;
      esac
      shift 2 ;;
    *) usage ;;
  esac
done
[[ -f $archive && $work_root == /* ]] || usage
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$ ]] || usage
[[ $digest =~ ^[0-9a-f]{64}$ && $source_commit =~ ^[0-9a-f]{40}$ && $architecture =~ ^[a-z0-9_.-]+$ && $libc =~ ^(musl|glibc)$ ]] || usage
script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
[[ $(sha256sum "$archive" | awk '{print $1}') == "$digest" ]] || {
  printf '%s\n' 'verify-package: checksum mismatch' >&2; exit 78; }
root="container-tools-${version}-${architecture}-${libc}-${source_commit}"
work="$work_root/${root}.$$"
mkdir -p -- "$work"
trap 'rm -rf -- "$work"' EXIT
tar -tzf "$archive" > "$work/names"
awk -v root="$root" 'BEGIN { ok=1 } $0 ~ /^\// || $0 ~ /(^|\/)\.\.?(\/|$)/ || index($0, root "/") != 1 { ok=0 } { n=$0; sub(/\/$/, "", n); if (seen[n]++) ok=0 } END { exit ok ? 0 : 1 }' "$work/names" || {
  printf '%s\n' 'verify-package: invalid archive path or duplicate entry' >&2; exit 78; }
tar -tvzf "$archive" | awk '$1 !~ /^[-d]/ { exit 1 }' || {
  printf '%s\n' 'verify-package: links or special archive entries are forbidden' >&2; exit 78; }
tar -xzf "$archive" -C "$work"
package_root="$work/$root"
LC_ALL=C sort -u "$work/names" | sed 's:/$::' > "$work/actual"
awk -v root="$root" '{ path=$0; sub(/\/$/, "", path); print path == "." ? root : root "/" path }' \
  "$script_dir/package-files.txt" | LC_ALL=C sort -u > "$work/expected"
if ! cmp -s "$work/actual" "$work/expected" ||
   ! cmp -s "$package_root/archive-files.txt" "$work/expected"; then
  printf '%s\n' 'verify-package: archive has missing or extra entries' >&2
  exit 78
fi
for path in README.md LICENSE-APACHE LICENSE-MIT archive.json archive-files.txt \
  vendor/tomlc17/PROVENANCE.md vendor/tomlc17/LICENSE vendor/yyjson/PROVENANCE.md vendor/yyjson/LICENSE \
  bin/container-tools bin/ct_exec.sh bin/ct_shell.sh bin/ct_instance_exec.sh bin/ct_mount_detector.sh bin/ct_args.sh \
  share/container-tools/release.json; do
  [[ -f $package_root/$path ]] || { printf 'verify-package: missing %s\n' "$path" >&2; exit 78; }
done
grep -Fqx "{\"schema\":\"container-tools.archive/v1\",\"version\":\"$version\",\"architecture\":\"$architecture\",\"libc\":\"$libc\",\"source_commit\":\"$source_commit\"}" "$package_root/archive.json" || {
  printf '%s\n' 'verify-package: archive identity mismatch' >&2; exit 78; }
release="$package_root/share/container-tools/release.json"
if ! grep -Fq "\"product_version\":\"$version\"" "$release" ||
  ! grep -Fq "\"source_commit\":\"$source_commit\"" "$release" ||
  ! grep -Fq "\"architecture\":\"$architecture\"" "$release"; then
  printf '%s\n' 'verify-package: release identity mismatch' >&2
  exit 78
fi
readelf -l "$package_root/bin/container-tools" | grep -Eq 'INTERP|DYNAMIC' && {
  printf '%s\n' 'verify-package: executable is not static' >&2; exit 78; }
case $architecture in
  x86_64) expected_machine='Advanced Micro Devices X86-64' ;;
  aarch64) expected_machine='AArch64' ;;
  ppc64le) expected_machine='PowerPC64' ;;
  *) expected_machine= ;;
esac
[[ -z $expected_machine ]] || readelf -h "$package_root/bin/container-tools" | grep -Fq "Machine:                           $expected_machine" || {
  printf '%s\n' 'verify-package: executable machine mismatch' >&2; exit 78; }
version_json=$("$package_root/bin/container-tools" --version --json)
verify_json=$("$package_root/bin/container-tools" package verify --json)
[[ $version_json == "$(<"$release")" && $verify_json == "$version_json" ]] || {
  printf '%s\n' 'verify-package: executable package identity mismatch' >&2; exit 78; }
printf '%s\n' 'container-tools package verified'
