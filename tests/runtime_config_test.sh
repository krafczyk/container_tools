#!/usr/bin/env bash
set -euo pipefail

work=${1:?pass a task-specific directory beneath /tmp/mkchad-v1}
root=${2:?pass the container_tools checkout}
root=$(realpath "$root")
work=$(realpath -m -- "$work")
[[ $work == /tmp/mkchad-v1/* ]] || {
  printf '%s\n' 'test directory must be beneath /tmp/mkchad-v1' >&2
  exit 2
}
[[ ! -e $work ]] || {
  printf '%s\n' 'test directory already exists' >&2
  exit 2
}

home="$work/home"
config="$home/.config/ct_runtime.conf"
mkdir -p "$home/.config"
cat > "$config" <<EOF
# Machine-local container storage.
CT_SINGULARITY_CACHE_DIR=$work/storage/singularity cache
CT_SINGULARITY_TMP_DIR=$work/storage/singularity tmp
CT_DOCKER_BUILD_CACHE_DIR=$work/storage/docker cache
CT_DOCKER_BUILD_TMP_DIR=$work/storage/docker tmp
EOF
chmod 600 "$config"

export HOME="$home"
export CT_RUNTIME_CFG="$config"
# shellcheck disable=SC2034
script_dir=$root
# shellcheck disable=SC1091
. "$root/ct_library.sh"

lock_holder=
cleanup() {
  if [[ -n $lock_holder ]]; then
    kill "$lock_holder" 2>/dev/null || true
    wait "$lock_holder" 2>/dev/null || true
  fi
}
trap cleanup EXIT

assert_config_rejected() {
  local rejected_config=$1 failure_message=$2
  if HOME="$home" CT_RUNTIME_CFG="$rejected_config" bash -c \
    'set -euo pipefail; script_dir=$1; . "$1/ct_library.sh"; load_runtime_config' bash "$root"; then
    printf '%s\n' "$failure_message" >&2
    exit 1
  fi
}

configure_runtime_storage apptainer
[[ $APPTAINER_CACHEDIR == "$work/storage/singularity cache" ]]
[[ $APPTAINER_TMPDIR == "$work/storage/singularity tmp" ]]
for directory in "$APPTAINER_CACHEDIR" "$APPTAINER_TMPDIR"; do
  [[ -d $directory && $(stat -Lc '%a' -- "$directory") == 700 ]] || {
    printf 'runtime directory is not private: %s\n' "$directory" >&2
    exit 1
  }
done

unset TMPDIR
configure_docker_build_storage x86_64
docker_cache_root="$work/storage/docker cache"
docker_namespace="$docker_cache_root/x86_64"
docker_current="$docker_namespace/current"
first_staging=$DOCKER_BUILD_CACHE_STAGING
[[ $TMPDIR == "$work/storage/docker tmp" ]]
[[ -d $TMPDIR && $(stat -Lc '%a' -- "$TMPDIR") == 700 ]]
[[ -d $docker_cache_root && $(stat -Lc '%a' -- "$docker_cache_root") == 700 ]]
[[ -d $docker_namespace && $(stat -Lc '%a' -- "$docker_namespace") == 700 ]]
[[ ${DOCKER_BUILD_CACHE_ARGS[*]} == "--cache-to type=local,dest=$first_staging,mode=max" ]]
: > "$first_staging/index.json"
: > "$first_staging/first-generation"
commit_docker_build_storage
[[ -f $docker_current/index.json && -f $docker_current/first-generation ]]
[[ ! -e $first_staging && ! -L $first_staging ]]
[[ -z ${DOCKER_BUILD_CACHE_LOCK_FD:-} ]]
discard_docker_build_storage

configure_docker_build_storage x86_64
second_staging=$DOCKER_BUILD_CACHE_STAGING
[[ ${DOCKER_BUILD_CACHE_ARGS[*]} == "--cache-from type=local,src=$docker_current --cache-to type=local,dest=$second_staging,mode=max" ]]
: > "$second_staging/index.json"
: > "$second_staging/second-generation"
commit_docker_build_storage
[[ -f $docker_current/second-generation && ! -e $docker_current/first-generation ]]
[[ ! -e $docker_namespace/.previous && ! -L $docker_namespace/.previous ]]

mv "$docker_current" "$docker_namespace/.previous"
mkdir "$docker_namespace/.next.interrupted"
: > "$docker_namespace/.next.interrupted/partial"
configure_docker_build_storage x86_64
discarded_staging=$DOCKER_BUILD_CACHE_STAGING
[[ -f $docker_current/second-generation ]]
[[ ! -e $docker_namespace/.next.interrupted && ! -L $docker_namespace/.next.interrupted ]]
: > "$discarded_staging/partial"
discard_docker_build_storage
[[ ! -e $discarded_staging && ! -L $discarded_staging ]]
[[ -f $docker_current/second-generation ]]
discard_docker_build_storage

for invalid_architecture in . .. ../outside; do
  if configure_docker_build_storage "$invalid_architecture"; then
    printf 'unsafe Docker cache architecture was accepted: %s\n' "$invalid_architecture" >&2
    exit 1
  fi
done

lock_marker="$work/cache-lock-held"
(
  exec 8> "$docker_namespace/.lock"
  flock 8
  : > "$lock_marker"
  sleep 10
) &
lock_holder=$!
for _ in {1..100}; do
  [[ -e $lock_marker ]] && break
  sleep 0.02
done
[[ -e $lock_marker ]] || {
  printf '%s\n' 'Docker cache lock fixture did not acquire the lock' >&2
  exit 1
}
if HOME="$home" CT_RUNTIME_CFG="$config" TMPDIR=/tmp CT_DOCKER_BUILD_LOCK_TIMEOUT=0.1 \
  bash -c 'set -euo pipefail; script_dir=$1; . "$1/ct_library.sh"; configure_docker_build_storage x86_64' \
  bash "$root"; then
  printf '%s\n' 'concurrent Docker cache build did not respect the lock timeout' >&2
  exit 1
fi
kill "$lock_holder"
wait "$lock_holder" 2>/dev/null || true
lock_holder=

override="$work/override cache"
mkdir -m 700 "$override"
APPTAINER_CACHEDIR=$override
APPTAINER_TMPDIR=/tmp
configure_runtime_storage apptainer
[[ $APPTAINER_CACHEDIR == "$override" && $APPTAINER_TMPDIR == /tmp ]] || {
  printf '%s\n' 'explicit native runtime environment did not override machine config' >&2
  exit 1
}
(
  TMPDIR=/tmp
  configure_runtime_storage docker
  [[ $TMPDIR == /tmp ]]
)

caller_cache="$work/caller singularity cache"
HOME="$home" CT_RUNTIME_CFG="$config" CT_SINGULARITY_CACHE_DIR="$caller_cache" \
  bash -c 'set -euo pipefail; script_dir=$1; . "$1/ct_library.sh"; configure_runtime_storage singularity; [[ $SINGULARITY_CACHEDIR == "$2" ]]' \
  bash "$root" "$caller_cache"

launcher_tmp="$work/launcher docker tmp"
launcher_config="$work/launcher.conf"
printf 'CT_DOCKER_BUILD_TMP_DIR=%s\n' "$launcher_tmp" > "$launcher_config"
chmod 600 "$launcher_config"
# shellcheck disable=SC2016
HOME="$home" CT_RUNTIME_CFG="$launcher_config" env -u TMPDIR bash -c '
  set -euo pipefail
  script_dir=$1
  . "$1/ct_library.sh"
  parse_explicit_mount_args() { :; }
  build_mount_args() { :; }
  build_env_args() { :; }
  launcher_preamble --docker
  [[ -z ${TMPDIR:-} && ! -e $2 ]]
' bash "$root" "$launcher_tmp"

safe_existing="$work/safe-existing"
mkdir -m 755 "$safe_existing"
ensure_private_runtime_directory "$safe_existing"
[[ $(stat -Lc '%a' -- "$safe_existing") == 700 ]]

unsafe_existing="$work/unsafe-existing"
mkdir -m 770 "$unsafe_existing"
if ensure_private_runtime_directory "$unsafe_existing"; then
  printf '%s\n' 'writable runtime directory was adopted' >&2
  exit 1
fi
[[ $(stat -Lc '%a' -- "$unsafe_existing") == 770 ]]

symlink_target="$work/symlink-target"
symlink_path="$work/symlink-runtime"
mkdir -m 700 "$symlink_target"
ln -s "$symlink_target" "$symlink_path"
if ensure_private_runtime_directory "$symlink_path"; then
  printf '%s\n' 'symlinked runtime directory was adopted' >&2
  exit 1
fi

unsafe_parent="$work/unsafe-parent"
mkdir -m 777 "$unsafe_parent"
if ensure_private_runtime_directory "$unsafe_parent/child"; then
  printf '%s\n' 'runtime directory beneath a writable ancestor was created' >&2
  exit 1
fi
[[ ! -e $unsafe_parent/child && ! -L $unsafe_parent/child ]]

symlink_parent_target="$work/symlink-parent-target"
symlink_parent="$work/symlink-parent"
mkdir -m 700 "$symlink_parent_target"
ln -s "$symlink_parent_target" "$symlink_parent"
if ensure_private_runtime_directory "$symlink_parent/child"; then
  printf '%s\n' 'runtime directory beneath a symlinked ancestor was created' >&2
  exit 1
fi

bad="$work/bad.conf"
printf '%s\n' 'UNKNOWN_RUNTIME_KEY=/tmp/unsafe' > "$bad"
chmod 600 "$bad"
assert_config_rejected "$bad" 'unknown runtime config key was accepted'

marker="$work/injected"
# shellcheck disable=SC2016
printf 'CT_SINGULARITY_CACHE_DIR=$(touch %s)\n' "$marker" > "$bad"
assert_config_rejected "$bad" 'shell expression in runtime config was accepted'
[[ ! -e $marker ]] || {
  printf '%s\n' 'runtime config evaluated shell input' >&2
  exit 1
}

printf '%s\n' \
  'CT_SINGULARITY_CACHE_DIR=/tmp/first' \
  'CT_SINGULARITY_CACHE_DIR=/tmp/second' > "$bad"
assert_config_rejected "$bad" 'duplicate runtime config key was accepted'

printf '%s\n' 'CT_SINGULARITY_CACHE_DIR' > "$bad"
assert_config_rejected "$bad" 'runtime config line without equals was accepted'

printf '%s\n' 'CT_SINGULARITY_CACHE_DIR=relative/path' > "$bad"
assert_config_rejected "$bad" 'relative runtime config path was accepted'

printf '%s\n' 'CT_DOCKER_BUILD_CACHE_DIR=/tmp/cache,unsafe' > "$bad"
assert_config_rejected "$bad" 'comma-containing Docker cache path was accepted'

printf '%s\n' \
  'CT_SINGULARITY_CACHE_DIR=/tmp/partially-applied' \
  'UNKNOWN_RUNTIME_KEY=/tmp/unsafe' > "$bad"
if HOME="$home" CT_RUNTIME_CFG="$bad" bash -c '
  set -euo pipefail
  script_dir=$1
  . "$1/ct_library.sh"
  if load_runtime_config; then exit 1; fi
  [[ -z ${CT_SINGULARITY_CACHE_DIR:-} ]]
' bash "$root"; then
  :
else
  printf '%s\n' 'failed config partially applied values' >&2
  exit 1
fi

chmod 666 "$bad"
printf '%s\n' "CT_SINGULARITY_CACHE_DIR=$work/other" > "$bad"
assert_config_rejected "$bad" 'writable runtime config was accepted'
chmod 600 "$bad"

unopenable_config="$work/unopenable.conf"
ln -s "$work/missing-config-target" "$unopenable_config"
assert_config_rejected "$unopenable_config" 'unopenable runtime config was treated as absent'

config_directory="$work/config-directory"
mkdir "$config_directory"
assert_config_rejected "$config_directory" 'runtime config directory was accepted'

symlink_config_target="$work/symlink-config-target"
symlink_config="$work/symlink-config"
printf 'CT_SINGULARITY_CACHE_DIR=%s\n' "$work/symlink-config-cache" > "$symlink_config_target"
chmod 600 "$symlink_config_target"
ln -s "$symlink_config_target" "$symlink_config"
HOME="$home" CT_RUNTIME_CFG="$symlink_config" bash -c \
  'set -euo pipefail; script_dir=$1; . "$1/ct_library.sh"; load_runtime_config; [[ -n $CT_SINGULARITY_CACHE_DIR ]]' \
  bash "$root"

slow_bin="$work/slow-bin"
mkdir "$slow_bin"
cat > "$slow_bin/stat" <<'EOF'
#!/usr/bin/env bash
exec /usr/bin/sleep 2
EOF
chmod +x "$slow_bin/stat"
SECONDS=0
if HOME="$home" CT_RUNTIME_CFG="$config" CT_RUNTIME_STORAGE_TIMEOUT=0.1 \
  PATH="$slow_bin:$PATH" bash -c \
  'set -euo pipefail; script_dir=$1; . "$1/ct_library.sh"; load_runtime_config' bash "$root"; then
  printf '%s\n' 'runtime storage metadata timeout was ignored' >&2
  exit 1
fi
(( SECONDS < 2 )) || {
  printf '%s\n' 'runtime storage metadata timeout was not bounded' >&2
  exit 1
}

HOME="$home" CT_RUNTIME_CFG="$work/missing.conf" bash -c \
  'set -euo pipefail; script_dir=$1; . "$1/ct_library.sh"; load_runtime_config' bash "$root"

configure_runtime_storage podman
if configure_runtime_storage unsupported; then
  printf '%s\n' 'unsupported runtime backend was accepted' >&2
  exit 1
fi

trap - EXIT
printf '%s\n' 'container-tools runtime config tests passed'
