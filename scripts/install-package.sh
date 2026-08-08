#!/usr/bin/env bash
# Install a verified archive into a package-managed prefix through one locked transaction.
set -euo pipefail

usage() {
  printf '%s\n' 'usage: install-package.sh --check|--apply|--verify --archive FILE --sha256 64_HEX --version VERSION --source-commit 40_HEX --architecture ARCH --libc musl|glibc --prefix ABSOLUTE_DIR [--recovery-dir ABSOLUTE_DIR]' >&2
  exit 64
}

mode=''
archive=''
digest=''
version=''
source_commit=''
architecture=''
libc=''
prefix=''
recovery=''
while (($#)); do
  case $1 in
    --check|--apply|--verify) [[ -z $mode ]] || usage; mode=${1#--}; shift ;;
    --archive|--sha256|--version|--source-commit|--architecture|--libc|--prefix|--recovery-dir)
      (($# >= 2)) || usage
      case $1 in
        --archive) archive=$2 ;; --sha256) digest=$2 ;; --version) version=$2 ;;
        --source-commit) source_commit=$2 ;; --architecture) architecture=$2 ;; --libc) libc=$2 ;;
        --prefix) prefix=$2 ;; --recovery-dir) recovery=$2 ;;
      esac
      shift 2 ;;
    *) usage ;;
  esac
done
[[ -n $mode ]] || usage
if [[ $mode == check ]]; then
  [[ $prefix == /* && -z $recovery ]] || usage
elif [[ $mode == apply ]]; then
  [[ $prefix == /* && $recovery == /* ]] || usage
else
  [[ $prefix == /* && -z $recovery ]] || usage
fi
script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
verify_args=(--archive "$archive" --sha256 "$digest" --version "$version" --source-commit "$source_commit" --architecture "$architecture" --libc "$libc")
package_bins=(container-tools ct_exec.sh ct_shell.sh ct_instance_exec.sh ct_mount_detector.sh ct_args.sh)

projection_is_owned() {
  local path=$1 target=$2
  [[ -L $path && $(readlink -- "$path") == "$target" ]]
}

validate_projections() {
  local name
  [[ -d $prefix/bin && ! -L $prefix/bin && -d $prefix/share && ! -L $prefix/share ]] || return 1
  for name in "${package_bins[@]}"; do
    projection_is_owned "$prefix/bin/$name" "../.container-tools/current/bin/$name" || return 1
  done
  projection_is_owned "$prefix/share/container-tools" ../.container-tools/current/share/container-tools
}

validate_projection_slots() {
  local directory name path
  for directory in "$prefix/bin" "$prefix/share"; do
    if [[ -e $directory || -L $directory ]]; then
      [[ -d $directory && ! -L $directory ]] || {
        printf 'install-package: unmanaged prefix collision: %s\n' "$directory" >&2
        return 1
      }
    fi
  done
  for name in "${package_bins[@]}"; do
    path="$prefix/bin/$name"
    if [[ -e $path || -L $path ]]; then
      projection_is_owned "$path" "../.container-tools/current/bin/$name" || {
        printf 'install-package: unmanaged prefix collision: %s\n' "$path" >&2
        return 1
      }
    fi
  done
  path="$prefix/share/container-tools"
  if [[ -e $path || -L $path ]]; then
    projection_is_owned "$path" ../.container-tools/current/share/container-tools || {
      printf 'install-package: unmanaged prefix collision: %s\n' "$path" >&2
      return 1
    }
  fi
}

if [[ $mode == check ]]; then
  "$script_dir/verify-package.sh" "${verify_args[@]}" >/dev/null
  validate_projection_slots
  exit
fi
if [[ $mode == verify ]]; then
  "$script_dir/verify-package.sh" "${verify_args[@]}" >/dev/null
  validate_projections || { printf '%s\n' 'install-package: prefix is incomplete' >&2; exit 78; }
  observed=$("$prefix/bin/container-tools" package verify --json) || exit 78
  [[ $observed == *"\"product_version\":\"$version\""* &&
     $observed == *"\"source_commit\":\"$source_commit\""* &&
     $observed == *"\"architecture\":\"$architecture\""* ]] || exit 78
  exit 0
fi

managed="$prefix/.container-tools"
validate_projection_slots
mkdir -p -- "$recovery" "$prefix"
chmod 700 -- "$recovery"
umask 077
exec 9>"$recovery/container-tools-install.lock"
flock -x 9
identity="$version-$architecture-$libc-$source_commit"
transaction="$recovery/$identity"
state="$transaction/state"
candidate="$transaction/candidate.tar.gz"
expected_state="expose:$digest"
mkdir -p -- "$transaction"
if [[ -f $state ]]; then
  [[ $(<"$state") == "$expected_state" && -f $candidate &&
     $(sha256sum "$candidate" | awk '{print $1}') == "$digest" ]] || {
    printf '%s\n' 'install-package: recovery state does not match the requested archive' >&2
    exit 78
  }
else
  rm -rf -- "$transaction/stage" "$candidate" "$candidate.tmp"
  cp -- "$archive" "$candidate.tmp"
  [[ $(sha256sum "$candidate.tmp" | awk '{print $1}') == "$digest" ]] || {
    printf '%s\n' 'install-package: archive changed while staging' >&2
    exit 78
  }
  mv -- "$candidate.tmp" "$candidate"
  mkdir -p -- "$transaction/stage"
  apply_verify_args=("${verify_args[@]}")
  apply_verify_args[1]=$candidate
  "$script_dir/verify-package.sh" "${apply_verify_args[@]}" --work-root "$transaction/stage" >/dev/null
  tar -xzf "$candidate" -C "$transaction/stage"
  printf '%s\n' "$expected_state" > "$state.tmp"
  mv -- "$state.tmp" "$state"
fi
[[ ${CT_PACKAGE_INSTALL_INTERRUPT_AT:-} != after-stage ]] || exit 99
versions="$managed/versions"
mkdir -p -- "$versions"
chmod 700 -- "$managed" "$versions"
archive_root="container-tools-${version}-${architecture}-${libc}-${source_commit}"
if [[ ! -d $versions/$identity ]]; then
  mv -- "$transaction/stage/$archive_root" "$versions/$identity"
fi
"$versions/$identity/bin/container-tools" package verify --json |
  grep -Fq "\"source_commit\":\"$source_commit\"" || {
    printf '%s\n' 'install-package: managed package identity mismatch' >&2
    exit 78
  }
mkdir -p -- "$prefix/bin" "$prefix/share"
for name in "${package_bins[@]}"; do
  path="$prefix/bin/$name"
  if [[ ! -e $path && ! -L $path ]]; then
    ln -s "../.container-tools/current/bin/$name" "$path"
  fi
done
path="$prefix/share/container-tools"
if [[ ! -e $path && ! -L $path ]]; then
  ln -s ../.container-tools/current/share/container-tools "$path"
fi
[[ ${CT_PACKAGE_INSTALL_INTERRUPT_AT:-} != after-links ]] || exit 99
ln -sfn "versions/$identity" "$managed/.next"
mv -Tf -- "$managed/.next" "$managed/current"
[[ ${CT_PACKAGE_INSTALL_INTERRUPT_AT:-} != after-expose ]] || exit 99
"$prefix/bin/container-tools" package verify --json | grep -Fq "\"source_commit\":\"$source_commit\"" || exit 78
rm -f -- "$state"
rm -rf -- "$transaction"
