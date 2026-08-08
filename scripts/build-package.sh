#!/usr/bin/env bash
# Build one deterministic, statically linked container-tools archive from an exact source tree.
set -euo pipefail

usage() {
  printf '%s\n' 'usage: build-package.sh --build-root ABSOLUTE_DIR --version VERSION --source-commit 40_HEX --output-dir ABSOLUTE_DIR --libc musl|glibc' >&2
  exit 64
}

build_root=''
version=''
source_commit=''
output_dir=''
libc=''
while (($#)); do
  case $1 in
    --build-root|--version|--source-commit|--output-dir|--libc)
      (($# >= 2)) || usage
      case $1 in
        --build-root) build_root=$2 ;; --version) version=$2 ;;
        --source-commit) source_commit=$2 ;; --output-dir) output_dir=$2 ;;
        --libc) libc=$2 ;;
      esac
      shift 2 ;;
    *) usage ;;
  esac
done
[[ $build_root == /* && $output_dir == /* && $build_root == /tmp/mkchad-v1/container-tools-c11/* ]] || usage
[[ $version =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$ ]] || usage
[[ $source_commit =~ ^[0-9a-f]{40}$ && $libc =~ ^(musl|glibc)$ ]] || usage

source_root=$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)
[[ $(git -C "$source_root" rev-parse HEAD) == "$source_commit" ]] || {
  printf '%s\n' 'build-package: requested source commit is not HEAD' >&2; exit 78; }
[[ -z $(git -C "$source_root" status --porcelain) ]] || {
  printf '%s\n' 'build-package: source tree is not clean' >&2; exit 78; }
mkdir -p -- "$build_root" "$output_dir"
architecture=$(uname -m | tr '[:upper:]' '[:lower:]')
archive_name="container-tools-${version}-${architecture}-${libc}-${source_commit}"
archive="$output_dir/$archive_name.tar.gz"
checksum="$archive.sha256"
[[ ! -e $archive && ! -e $checksum ]] || {
  printf '%s\n' 'build-package: output already exists' >&2; exit 78; }

work="$build_root/$archive_name"
[[ ! -e $work ]] || { printf '%s\n' 'build-package: build directory already exists' >&2; exit 78; }
mkdir -p -- "$work/root"
trap 'rm -rf -- "$work"' EXIT
compiler=${CC:-cc}
if [[ $libc == musl ]]; then
  compiler=${CT_MUSL_CC:-musl-gcc}
  command -v "$compiler" >/dev/null || {
    printf '%s\n' 'build-package: requested musl compiler is unavailable' >&2
    exit 69
  }
else
  command -v "$compiler" >/dev/null || {
    printf '%s\n' 'build-package: requested C compiler is unavailable' >&2
    exit 69
  }
  "$compiler" -dM -E -include features.h - </dev/null | grep -Fq '__GLIBC__' || {
    printf '%s\n' 'build-package: requested glibc does not match the selected compiler' >&2
    exit 78
  }
fi
cmake -S "$source_root" -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$compiler" -DCONTAINER_TOOLS_PRODUCT_VERSION="$version" \
  -DCONTAINER_TOOLS_STATIC=ON
cmake --build "$work/build"
cmake --install "$work/build" --prefix "$work/root"
readelf -l "$work/root/bin/container-tools" | grep -Eq 'INTERP|DYNAMIC' && {
  printf '%s\n' 'build-package: production executable is not static' >&2; exit 78; }
release="$work/root/share/container-tools/release.json"
if ! grep -Fq "\"product_version\":\"$version\"" "$release" ||
  ! grep -Fq "\"source_commit\":\"$source_commit\"" "$release" ||
  ! grep -Fq "\"architecture\":\"$architecture\"" "$release"; then
  printf '%s\n' 'build-package: installed release identity mismatch' >&2
  exit 78
fi
package_root="$work/$archive_name"
mv -- "$work/root" "$package_root"
cp -- "$source_root/README.md" "$source_root/LICENSE-APACHE" "$source_root/LICENSE-MIT" "$package_root/"
mkdir -p -- "$package_root/vendor/tomlc17" "$package_root/vendor/yyjson"
cp -- "$source_root/vendor/tomlc17/PROVENANCE.md" "$source_root/vendor/tomlc17/LICENSE" "$package_root/vendor/tomlc17/"
cp -- "$source_root/vendor/yyjson/PROVENANCE.md" "$source_root/vendor/yyjson/LICENSE" "$package_root/vendor/yyjson/"
printf '{"schema":"container-tools.archive/v1","version":"%s","architecture":"%s","libc":"%s","source_commit":"%s"}\n' \
  "$version" "$architecture" "$libc" "$source_commit" > "$package_root/archive.json"
awk -v root="$archive_name" '{ path=$0; sub(/\/$/, "", path); print path == "." ? root : root "/" path }' \
  "$source_root/scripts/package-files.txt" | LC_ALL=C sort -u > "$package_root/archive-files.txt"
(cd "$work" && find "$archive_name" -print | LC_ALL=C sort > "$work/archive-files.actual")
cmp -s "$work/archive-files.actual" "$package_root/archive-files.txt" || {
  printf '%s\n' 'build-package: installed package layout is not allowlisted' >&2
  exit 78
}
timestamp=$(git -C "$source_root" show -s --format=%ct "$source_commit")
tar --format=posix --sort=name --mtime="@$timestamp" --owner=0 --group=0 --numeric-owner \
  -C "$work" -czf "$archive" "$archive_name"
(cd "$output_dir" && sha256sum "$(basename "$archive")" > "$(basename "$checksum")")
trap - EXIT
rm -rf -- "$work"
printf '%s\n' "$archive"
