# shellcheck shell=bash

# Print one runtime-storage diagnostic and return failure.
runtime_config_error() {
  printf 'Error: container runtime config: %s\n' "$*" >&2
  return 1
}

# Run one potentially blocking storage operation with a bounded deadline.
runtime_storage_command() {
  local timeout_seconds=${CT_RUNTIME_STORAGE_TIMEOUT:-10}
  [[ $timeout_seconds =~ ^[0-9]+([.][0-9]+)?$ && ! $timeout_seconds =~ ^0+([.]0+)?$ ]] \
    || runtime_config_error "CT_RUNTIME_STORAGE_TIMEOUT must be a positive number of seconds" || return 1
  LC_ALL=C timeout --foreground --kill-after=2s "${timeout_seconds}s" "$@"
}

# Return 0 when a path exists (including a symlink), 1 when absent, and 2 when
# bounded metadata inspection itself fails.
runtime_path_exists() {
  local status
  if runtime_storage_command stat -c '%F' -- "$1" >/dev/null 2>&1; then
    return 0
  else
    status=$?
  fi
  (( status == 1 )) && return 1
  runtime_config_error "cannot inspect runtime path: $1"
  return 2
}

# Open, validate, and read one config file through the same descriptor. Exit 66
# means the optional path was absent; every other failure is fail-closed.
read_runtime_config_file() {
  # shellcheck disable=SC2016
  runtime_storage_command bash -c '
    set -euo pipefail
    if [[ ! -e $1 && ! -L $1 ]]; then exit 66; fi
    exec 3< "$1"
    metadata=$(stat -Lc "%F|%u|%a" -- /proc/self/fd/3)
    IFS="|" read -r kind uid mode <<< "$metadata"
    [[ $kind == "regular file" ]] || { printf "Error: container runtime config: not a regular file: %s\n" "$1" >&2; exit 1; }
    [[ $uid == "$2" ]] || { printf "Error: container runtime config: must be current-user-owned: %s\n" "$1" >&2; exit 1; }
    (( (8#$mode & 022) == 0 )) || { printf "Error: container runtime config: must not be group- or world-writable: %s\n" "$1" >&2; exit 1; }
    cat <&3
  ' bash "$1" "$EUID"
}

# Reject symlinked or cross-user-writable ancestors. Sticky shared roots such as
# /tmp remain valid because another user cannot replace this user's child entry.
runtime_path_ancestors_are_safe() {
  local ancestor=${1%/*} kind mode metadata status
  [[ -n $ancestor ]] || ancestor=/
  while :; do
    if metadata=$(runtime_storage_command stat -c '%F|%a' -- "$ancestor" 2>/dev/null); then
      IFS='|' read -r kind mode <<< "$metadata"
      [[ $kind == directory ]] \
        || runtime_config_error "runtime path ancestor is not a plain directory: $ancestor" || return 1
      if (( (8#$mode & 022) != 0 && (8#$mode & 01000) == 0 )); then
        runtime_config_error "runtime path ancestor is group- or world-writable: $ancestor"
        return 1
      fi
    else
      status=$?
      (( status == 1 )) \
        || runtime_config_error "cannot inspect runtime path ancestor: $ancestor" || return 1
    fi
    [[ $ancestor == / ]] && return 0
    ancestor=${ancestor%/*}
    [[ -n $ancestor ]] || ancestor=/
  done
}

# Load optional KEY=ABSOLUTE_PATH machine defaults. Caller-supplied CT_* values
# win; malformed input returns failure without partially applying file values.
load_runtime_config() {
  [[ ${CT_RUNTIME_CONFIG_LOADED:-0} -eq 0 ]] || return 0

  CT_RUNTIME_CFG=${CT_RUNTIME_CFG:-${HOME}/.config/ct_runtime.conf}
  local config_status config_contents
  if config_contents=$(read_runtime_config_file "$CT_RUNTIME_CFG"); then
    :
  else
    config_status=$?
    if (( config_status == 66 )); then
      CT_RUNTIME_CONFIG_LOADED=1
      return 0
    fi
    runtime_config_error "cannot validate and read: $CT_RUNTIME_CFG"
    return 1
  fi

  local line key value
  local -A seen=() parsed=()
  while IFS= read -r line || [[ -n $line ]]; do
    line=${line%$'\r'}
    [[ $line =~ ^[[:space:]]*$ || $line =~ ^[[:space:]]*# ]] && continue
    [[ $line == *=* ]] || runtime_config_error "expected KEY=ABSOLUTE_PATH: $line" || return 1
    key=${line%%=*}
    value=${line#*=}
    case $key in
      CT_SINGULARITY_CACHE_DIR|CT_SINGULARITY_TMP_DIR|CT_DOCKER_BUILD_CACHE_DIR|CT_DOCKER_BUILD_TMP_DIR) ;;
      *) runtime_config_error "unknown key: $key" || return 1 ;;
    esac
    [[ -z ${seen[$key]+present} ]] || runtime_config_error "duplicate key: $key" || return 1
    [[ $value == /* && $value != *$'\n'* ]] \
      || runtime_config_error "$key requires an absolute single-line path" || return 1
    if [[ $key == CT_DOCKER_BUILD_CACHE_DIR && $value == *,* ]]; then
      runtime_config_error "$key cannot contain a comma"
      return 1
    fi
    seen[$key]=1
    parsed[$key]=$value
  done <<< "$config_contents"

  for key in "${!parsed[@]}"; do
    if [[ -z ${!key:-} ]]; then
      printf -v "$key" '%s' "${parsed[$key]}"
    fi
  done

  CT_RUNTIME_CONFIG_LOADED=1
}

# Create or adopt one configured directory. Existing symlinks or directories
# that were writable by another user fail closed; safe directories become 0700.
ensure_private_runtime_directory() {
  local directory=$1 directory_kind directory_uid directory_mode directory_stat status
  [[ $directory == /* && $directory != *$'\n'* \
    && $directory != */../* && $directory != */./* \
    && $directory != */.. && $directory != */. ]] \
    || runtime_config_error "runtime directory must be absolute" || return 1
  runtime_path_ancestors_are_safe "$directory" || return 1

  if directory_stat=$(runtime_storage_command stat -c '%F|%u|%a' -- "$directory" 2>/dev/null); then
    :
  else
    status=$?
    if (( status != 1 )); then
      runtime_config_error "cannot inspect runtime directory: $directory"
      return 1
    fi
    runtime_storage_command install -d -m 700 -- "$directory" \
      || runtime_config_error "cannot create runtime directory: $directory" || return 1
    directory_stat=$(runtime_storage_command stat -c '%F|%u|%a' -- "$directory") \
      || runtime_config_error "cannot inspect runtime directory: $directory" || return 1
    runtime_path_ancestors_are_safe "$directory" || return 1
  fi

  IFS='|' read -r directory_kind directory_uid directory_mode <<< "$directory_stat"
  [[ $directory_kind == directory ]] \
    || runtime_config_error "runtime path is not a plain directory: $directory" || return 1
  [[ $directory_uid == "$EUID" ]] \
    || runtime_config_error "runtime directory must be current-user-owned: $directory" || return 1
  (( (8#$directory_mode & 022) == 0 )) \
    || runtime_config_error "runtime directory must not be group- or world-writable: $directory" || return 1
  if [[ $directory_mode != 700 ]]; then
    runtime_storage_command chmod 700 -- "$directory" \
      || runtime_config_error "cannot make runtime directory private: $directory" || return 1
  fi
}

# Export a native runtime variable. Explicit native values remain untouched;
# only a CT_* fallback is created, ownership-checked, and made private.
configure_runtime_environment_directory() {
  local variable=$1 fallback=${2:-}
  if [[ -n ${!variable:-} ]]; then
    export "${variable?}"
    return 0
  fi
  [[ -n $fallback ]] || return 0
  ensure_private_runtime_directory "$fallback" || return 1
  printf -v "$variable" '%s' "$fallback"
  export "${variable?}"
}

# Apply configured cache/temp defaults for a named supported backend. Returns
# failure for malformed config, unsafe directories, or an unknown backend.
configure_runtime_storage() {
  local backend=$1 variable_prefix
  load_runtime_config || return 1

  case $backend in
    singularity|apptainer)
      variable_prefix=${backend^^}
      configure_runtime_environment_directory \
        "${variable_prefix}_CACHEDIR" "${CT_SINGULARITY_CACHE_DIR:-}" || return 1
      configure_runtime_environment_directory \
        "${variable_prefix}_TMPDIR" "${CT_SINGULARITY_TMP_DIR:-}" || return 1
      ;;
    docker)
      configure_runtime_environment_directory TMPDIR "${CT_DOCKER_BUILD_TMP_DIR:-}" || return 1
      ;;
    podman) ;;
    *) runtime_config_error "unsupported backend: $backend" || return 1 ;;
  esac
}

# Release the Buildx cache lock held by configure_docker_build_storage.
release_docker_build_storage_lock() {
  [[ -n ${DOCKER_BUILD_CACHE_LOCK_FD:-} ]] || return 0
  local lock_fd=$DOCKER_BUILD_CACHE_LOCK_FD status=0
  DOCKER_BUILD_CACHE_LOCK_FD=
  flock -u "$lock_fd" || status=$?
  exec {lock_fd}>&-
  return "$status"
}

# Prepare serialized Buildx import/export arguments for one architecture. A
# caller must commit after success and arrange discard on every exit path.
configure_docker_build_storage() {
  local architecture=$1 cache_namespace current previous stale path_status recovery_status
  local lock_timeout=${CT_DOCKER_BUILD_LOCK_TIMEOUT:-30}
  [[ $architecture =~ ^[A-Za-z0-9._-]+$ ]] \
    || runtime_config_error "unsafe Docker cache architecture: $architecture" || return 1
  [[ $architecture != . && $architecture != .. ]] \
    || runtime_config_error "unsafe Docker cache architecture: $architecture" || return 1
  [[ $lock_timeout =~ ^[0-9]+([.][0-9]+)?$ && ! $lock_timeout =~ ^0+([.]0+)?$ ]] \
    || runtime_config_error "CT_DOCKER_BUILD_LOCK_TIMEOUT must be a positive number of seconds" || return 1

  configure_runtime_storage docker || return 1
  DOCKER_BUILD_CACHE_ARGS=()
  DOCKER_BUILD_CACHE_NAMESPACE=
  DOCKER_BUILD_CACHE_CURRENT=
  DOCKER_BUILD_CACHE_STAGING=
  DOCKER_BUILD_CACHE_LOCK_FD=
  [[ -n ${CT_DOCKER_BUILD_CACHE_DIR:-} ]] || return 0
  [[ $CT_DOCKER_BUILD_CACHE_DIR != *,* ]] \
    || runtime_config_error "CT_DOCKER_BUILD_CACHE_DIR cannot contain a comma" || return 1

  ensure_private_runtime_directory "$CT_DOCKER_BUILD_CACHE_DIR" || return 1
  cache_namespace="$CT_DOCKER_BUILD_CACHE_DIR/$architecture"
  ensure_private_runtime_directory "$cache_namespace" || return 1
  exec {DOCKER_BUILD_CACHE_LOCK_FD}> "$cache_namespace/.lock" \
    || runtime_config_error "cannot open Docker cache lock: $cache_namespace/.lock" || return 1
  if ! flock -w "$lock_timeout" "$DOCKER_BUILD_CACHE_LOCK_FD"; then
    release_docker_build_storage_lock
    runtime_config_error "timed out waiting for Docker cache lock: $architecture"
    return 1
  fi

  current="$cache_namespace/current"
  previous="$cache_namespace/.previous"
  if runtime_path_exists "$previous"; then
    recovery_status=0
    if runtime_path_exists "$current"; then
      runtime_storage_command rm -rf -- "$previous" || recovery_status=$?
    else
      path_status=$?
      if (( path_status == 1 )); then
        runtime_storage_command mv -T -- "$previous" "$current" || recovery_status=$?
      else
        recovery_status=$path_status
      fi
    fi
    if (( recovery_status != 0 )); then
      release_docker_build_storage_lock
      runtime_config_error "cannot recover Docker cache generation: $architecture"
      return 1
    fi
  else
    path_status=$?
    if (( path_status != 1 )); then
      release_docker_build_storage_lock
      return 1
    fi
  fi
  for stale in "$cache_namespace"/.next.*; do
    if runtime_path_exists "$stale"; then
      :
    else
      path_status=$?
      (( path_status == 1 )) && continue
      release_docker_build_storage_lock
      return 1
    fi
    if ! runtime_storage_command rm -rf -- "$stale"; then
      release_docker_build_storage_lock
      runtime_config_error "cannot remove stale Docker cache generation: $stale"
      return 1
    fi
  done

  if runtime_path_exists "$current"; then
    ensure_private_runtime_directory "$current" || {
      release_docker_build_storage_lock
      return 1
    }
    if runtime_storage_command test -f "$current/index.json"; then
      DOCKER_BUILD_CACHE_ARGS+=(--cache-from "type=local,src=$current")
    else
      path_status=$?
      if (( path_status != 1 )); then
        release_docker_build_storage_lock
        runtime_config_error "cannot inspect Docker cache index: $current/index.json"
        return 1
      fi
    fi
  else
    path_status=$?
    if (( path_status != 1 )); then
      release_docker_build_storage_lock
      return 1
    fi
  fi

  DOCKER_BUILD_CACHE_NAMESPACE=$cache_namespace
  DOCKER_BUILD_CACHE_CURRENT=$current
  DOCKER_BUILD_CACHE_STAGING="$cache_namespace/.next.$$.$RANDOM"
  ensure_private_runtime_directory "$DOCKER_BUILD_CACHE_STAGING" || {
    release_docker_build_storage_lock
    return 1
  }
  DOCKER_BUILD_CACHE_ARGS+=(--cache-to "type=local,dest=$DOCKER_BUILD_CACHE_STAGING,mode=max")
}

# Promote a completed Buildx cache export and release its architecture lock.
# Failure leaves a recoverable previous generation and returns nonzero.
commit_docker_build_storage() {
  [[ -n ${DOCKER_BUILD_CACHE_STAGING:-} ]] || return 0
  local path_status previous="$DOCKER_BUILD_CACHE_NAMESPACE/.previous"
  runtime_storage_command test -f "$DOCKER_BUILD_CACHE_STAGING/index.json" \
    || runtime_config_error "Docker build did not produce a local cache index" || return 1
  runtime_storage_command rm -rf -- "$previous" || return 1
  if runtime_path_exists "$DOCKER_BUILD_CACHE_CURRENT"; then
    runtime_storage_command mv -T -- "$DOCKER_BUILD_CACHE_CURRENT" "$previous" || return 1
  else
    path_status=$?
    (( path_status == 1 )) || return 1
  fi
  if ! runtime_storage_command mv -T -- "$DOCKER_BUILD_CACHE_STAGING" "$DOCKER_BUILD_CACHE_CURRENT"; then
    if runtime_path_exists "$previous"; then
      runtime_storage_command mv -T -- "$previous" "$DOCKER_BUILD_CACHE_CURRENT" || true
    fi
    return 1
  fi
  DOCKER_BUILD_CACHE_STAGING=
  runtime_storage_command rm -rf -- "$previous" || return 1
  release_docker_build_storage_lock
}

# Remove an incomplete Buildx export and release its architecture lock. Safe to
# call unconditionally from an EXIT trap.
discard_docker_build_storage() {
  local path_status status=0
  if [[ -n ${DOCKER_BUILD_CACHE_STAGING:-} ]]; then
    if runtime_path_exists "$DOCKER_BUILD_CACHE_STAGING"; then
      runtime_storage_command rm -rf -- "$DOCKER_BUILD_CACHE_STAGING" || status=$?
    else
      path_status=$?
      (( path_status == 1 )) || status=$path_status
    fi
  fi
  DOCKER_BUILD_CACHE_STAGING=
  release_docker_build_storage_lock || status=$?
  return "$status"
}

# Host-projection records intentionally contain only selection data. The policy
# version changes whenever their meaning or their key material changes.
CT_HOST_PROJECTION_POLICY_VERSION='host-projection-v1'
CT_HOST_PROJECTION_PROFILE_VERSION='ct-host-projection-profile-v1'
CT_HOST_PROJECTION_RECORD_VERSION=ct-host-projection-selection-v1
CT_HOST_PROJECTION_RECORD_MAX_BYTES=1048576
CT_HOST_PROJECTION_RECORD_MAX_PAIRS=4096

# Decode the octal escapes used in /proc/*/mountinfo path fields.
ct_host_projection_unescape_mountinfo_path() {
  local value=$1
  value=${value//\\040/ }
  value=${value//\\011/$'\t'}
  value=${value//\\012/$'\n'}
  value=${value//\\134/\\}
  printf '%s' "$value"
}

# Return whether a filesystem is a host kernel API that must not be projected.
ct_host_projection_kernel_filesystem() {
  case "$1" in
    proc|sysfs|devtmpfs|devpts|securityfs|cgroup|cgroup2|pstore|efivarfs|debugfs|tracefs|configfs|fusectl|mqueue|hugetlbfs)
      return 0
      ;;
    *) return 1 ;;
  esac
}

# Return whether a path is inside a host kernel API namespace.
ct_host_projection_kernel_path() {
  local path=$1 root=${CT_HOST_PROJECTION_SOURCE_ROOT:-/} relative
  if [[ $root == / ]]; then
    relative=$path
  elif [[ $path == "$root" ]]; then
    relative=/
  elif [[ $path == "$root"/* ]]; then
    relative=/${path#"$root"/}
  else
    return 0
  fi
  case "$relative" in
    /proc|/proc/*|/sys|/sys/*|/dev|/dev/*) return 0 ;;
    *) return 1 ;;
  esac
}

# Return whether a lexical host source is eligible for generated projection.
ct_host_projection_source_eligible() {
  local source=$1 filesystem=${2:-}
  [[ $source == /* ]] || return 1
  ct_host_projection_kernel_path "$source" && return 1
  [[ -z $filesystem ]] || ! ct_host_projection_kernel_filesystem "$filesystem"
}

# Read one mountinfo snapshot for projection only. This does not affect the
# existing mount detector, whose output and filtering remain authoritative for
# regular same-path binds.
ct_host_projection_read_mountinfo() {
  local mountinfo=${CT_HOST_PROJECTION_MOUNTINFO:-/proc/self/mountinfo}
  local root=${CT_HOST_PROJECTION_SOURCE_ROOT:-/} line dash_index path filesystem
  local -a fields=()
  CT_HOST_PROJECTION_MOUNT_PATHS=()
  CT_HOST_PROJECTION_MOUNT_FILESYSTEMS=()
  declare -gA CT_HOST_PROJECTION_MOUNT_TYPES=()
  [[ -r $mountinfo ]] || return 1

  while IFS= read -r line || [[ -n $line ]]; do
    IFS=' ' read -r -a fields <<< "$line"
    (( ${#fields[@]} >= 7 )) || continue
    dash_index=6
    while (( dash_index < ${#fields[@]} )) && [[ ${fields[dash_index]} != - ]]; do
      ((dash_index++))
    done
    (( dash_index + 1 < ${#fields[@]} )) || continue
    path=$(ct_host_projection_unescape_mountinfo_path "${fields[4]}")
    filesystem=${fields[dash_index + 1]}
    if [[ $root != / && $path == / ]]; then
      path=$root
    fi
    [[ $path == /* ]] || continue
    CT_HOST_PROJECTION_MOUNT_PATHS+=("$path")
    CT_HOST_PROJECTION_MOUNT_FILESYSTEMS+=("$filesystem")
    CT_HOST_PROJECTION_MOUNT_TYPES["$path"]=$filesystem
  done < "$mountinfo"
}

# Populate a deterministic source/target/destination plan from associative
# candidates. All generated destinations are derived here rather than trusted
# from a record.
ct_host_projection_finalize_candidates() {
  local strategy=$1 source target
  local -a sorted=()
  CT_HOST_PROJECTION_SOURCES=()
  CT_HOST_PROJECTION_TARGETS=()
  CT_HOST_PROJECTION_DESTINATIONS=()
  CT_HOST_PROJECTION_STRATEGY=$strategy
  CT_HOST_PROJECTION_COMPLETE=${CT_HOST_PROJECTION_COMPLETE:-complete}
  mapfile -d '' -t sorted < <(printf '%s\0' "${!CT_HOST_PROJECTION_CANDIDATES[@]}" | LC_ALL=C sort -z)
  for source in "${sorted[@]}"; do
    target=${CT_HOST_PROJECTION_CANDIDATES[$source]}
    CT_HOST_PROJECTION_SOURCES+=("$source")
    CT_HOST_PROJECTION_TARGETS+=("$target")
    if [[ $source == / ]]; then
      CT_HOST_PROJECTION_DESTINATIONS+=(/host)
    else
      CT_HOST_PROJECTION_DESTINATIONS+=("/host$source")
    fi
  done
}

# Add one existing source to a plan, resolving directory symlinks semantically.
ct_host_projection_add_candidate() {
  local source=$1 filesystem=${2:-} target
  ct_host_projection_source_eligible "$source" "$filesystem" || {
    CT_HOST_PROJECTION_COMPLETE=partial
    return 0
  }
  if ! target=$(realpath -e -- "$source" 2>/dev/null); then
    CT_HOST_PROJECTION_COMPLETE=partial
    return 0
  fi
  CT_HOST_PROJECTION_CANDIDATES["$source"]=$target
}

# Build the non-recursive root strategy: root plus every eligible mountpoint.
ct_host_projection_build_direct() {
  local root=${CT_HOST_PROJECTION_SOURCE_ROOT:-/} index source filesystem
  declare -gA CT_HOST_PROJECTION_CANDIDATES=()
  CT_HOST_PROJECTION_COMPLETE=complete
  ct_host_projection_read_mountinfo || return 1
  ct_host_projection_add_candidate "$root"
  for index in "${!CT_HOST_PROJECTION_MOUNT_PATHS[@]}"; do
    source=${CT_HOST_PROJECTION_MOUNT_PATHS[index]}
    filesystem=${CT_HOST_PROJECTION_MOUNT_FILESYSTEMS[index]}
    [[ $source == "$root" ]] && continue
    # The non-recursive root bind already excludes kernel API mounts. Their
    # deliberate exclusion is not a failed direct projection candidate.
    ct_host_projection_source_eligible "$source" "$filesystem" || continue
    ct_host_projection_add_candidate "$source" "$filesystem"
  done
  ct_host_projection_finalize_candidates direct
}

# Return whether a recursive fallback candidate would import an excluded mount.
ct_host_projection_has_excluded_descendant() {
  local source=$1 index mounted filesystem
  for index in "${!CT_HOST_PROJECTION_MOUNT_PATHS[@]}"; do
    mounted=${CT_HOST_PROJECTION_MOUNT_PATHS[index]}
    filesystem=${CT_HOST_PROJECTION_MOUNT_FILESYSTEMS[index]}
    [[ $mounted == "$source"/* ]] || continue
    ct_host_projection_source_eligible "$mounted" "$filesystem" || return 0
  done
  return 1
}

# Build the recursive fallback strategy from top-level directories/symlinks and
# separately mounted descendants. This enumeration is cold-path only.
ct_host_projection_build_fallback() {
  local root=${CT_HOST_PROJECTION_SOURCE_ROOT:-/} glob_root source filesystem index
  local -a top_level=()
  declare -gA CT_HOST_PROJECTION_CANDIDATES=()
  CT_HOST_PROJECTION_COMPLETE=complete
  ct_host_projection_read_mountinfo || return 1

  [[ $root == / ]] && glob_root= || glob_root=$root
  for source in "$glob_root"/* "$glob_root"/.[!.]* "$glob_root"/..?*; do
    [[ -d $source || -L $source ]] || continue
    top_level+=("$source")
  done
  for source in "${top_level[@]}"; do
    filesystem=${CT_HOST_PROJECTION_MOUNT_TYPES[$source]:-}
    if ct_host_projection_has_excluded_descendant "$source"; then
      CT_HOST_PROJECTION_COMPLETE=partial
      continue
    fi
    ct_host_projection_add_candidate "$source" "$filesystem"
  done
  for index in "${!CT_HOST_PROJECTION_MOUNT_PATHS[@]}"; do
    source=${CT_HOST_PROJECTION_MOUNT_PATHS[index]}
    filesystem=${CT_HOST_PROJECTION_MOUNT_FILESYSTEMS[index]}
    [[ $source == "$root" ]] && continue
    ct_host_projection_has_excluded_descendant "$source" && {
      CT_HOST_PROJECTION_COMPLETE=partial
      continue
    }
    ct_host_projection_add_candidate "$source" "$filesystem"
  done
  ct_host_projection_finalize_candidates fallback
}

# Return the cache directory, using node-local runtime state when available.
ct_host_projection_cache_directory() {
  if [[ -n ${CT_HOST_PROJECTION_CACHE_ROOT:-} ]]; then
    printf '%s' "$CT_HOST_PROJECTION_CACHE_ROOT"
  elif [[ -n ${XDG_RUNTIME_DIR:-} ]]; then
    printf '%s/container-tools/host-projection-v1' "$XDG_RUNTIME_DIR"
  else
    printf '/tmp/container-tools-%s/host-projection-v1' "$EUID"
  fi
}

# Build a stable NUL-framed selection key without topology or invocation binds.
ct_host_projection_selection_key() {
  local backend=$1 image=$2 projection_options=$3 requested_group_mode=$4
  local hostname=${CT_HOST_PROJECTION_HOSTNAME:-} executable=${CT_HOST_PROJECTION_EXECUTABLE:-}
  local endpoint=${CT_HOST_PROJECTION_ENDPOINT:-} boot_id=${CT_HOST_PROJECTION_BOOT_ID:-}
  local uid=${CT_HOST_PROJECTION_UID:-$EUID} gid=${CT_HOST_PROJECTION_GID:-$(id -g)}
  local digest group
  local -a groups=()
  [[ -n $hostname ]] || hostname=${HOSTNAME:-$(< /etc/hostname)}
  [[ -n $executable ]] || executable=$(command -v -- "$backend" 2>/dev/null || true)
  [[ -n $executable ]] && executable=$(realpath -m -- "$executable")
  if [[ -z $endpoint ]]; then
    case "$backend" in
      docker) endpoint=${DOCKER_HOST:-local} ;;
      podman) endpoint=${CONTAINER_HOST:-${DOCKER_HOST:-local}} ;;
      *) endpoint=local ;;
    esac
  fi
  [[ -n $boot_id ]] || boot_id=$(< /proc/sys/kernel/random/boot_id)
  if [[ -n ${CT_HOST_PROJECTION_GROUPS:-} ]]; then
    mapfile -t groups < <(tr ' ' '\n' <<< "$CT_HOST_PROJECTION_GROUPS" | LC_ALL=C sort -n)
  else
    mapfile -t groups < <(id -G | tr ' ' '\n' | LC_ALL=C sort -n)
  fi
  digest=$({
    printf '%s\0' "$CT_HOST_PROJECTION_POLICY_VERSION" "$backend" "$hostname" "$executable" "$endpoint"
    printf '%s\0' "$image" "$projection_options" "$uid" "$gid" "$requested_group_mode" "$boot_id"
    for group in "${groups[@]}"; do printf '%s\0' "$group"; done
  } | sha256sum)
  CT_HOST_PROJECTION_SELECTION_KEY=${digest%% *}
  [[ $CT_HOST_PROJECTION_SELECTION_KEY =~ ^[0-9a-f]{64}$ ]]
}

# Hash semantic generated-bind fields for persistent instance profiles. The
# tuple deliberately excludes filesystem presentation that can change on a
# network remount without changing the selected projection.
ct_host_projection_profile_digest() {
  local backend=${1:-${TOOL[0]:-generic}} digest index recursion_policy
  case "$backend" in
    docker|podman|generic) recursion_policy=non-recursive ;;
    singularity|apptainer) recursion_policy=runtime-default ;;
    *) return 1 ;;
  esac
  digest=$({
    printf '%s\0' "$CT_HOST_PROJECTION_PROFILE_VERSION" "$CT_HOST_PROJECTION_POLICY_VERSION"
    printf '%s\0' "${CT_HOST_PROJECTION_STRATEGY:-none}" "$recursion_policy"
    for index in "${!CT_HOST_PROJECTION_SOURCES[@]}"; do
      printf '%s\0' "${CT_HOST_PROJECTION_STRATEGY:-none}" "${CT_HOST_PROJECTION_SOURCES[index]}"
      printf '%s\0' "${CT_HOST_PROJECTION_TARGETS[index]}" "${CT_HOST_PROJECTION_DESTINATIONS[index]}"
      printf '%s\0' writable "$recursion_policy"
    done
  } | sha256sum)
  CT_HOST_PROJECTION_PROFILE_DIGEST=${digest%% *}
  [[ $CT_HOST_PROJECTION_PROFILE_DIGEST =~ ^[0-9a-f]{64}$ ]]
}

# Return whether a closed record scalar is safe to serialize as one field.
ct_host_projection_record_scalar() {
  [[ $1 =~ ^[A-Za-z0-9._-]+$ ]]
}

# Persist one selection record atomically. Fallback values contain only the
# selected lexical/resolved pairs, never mount topology or rendered argv.
ct_host_projection_cache_write() {
  local key=$1 strategy=$2 completeness=$3 group_mode=$4 reason=$5
  local directory temporary size index
  local -a sources=("${CT_HOST_PROJECTION_SOURCES[@]:-}") targets=("${CT_HOST_PROJECTION_TARGETS[@]:-}")
  [[ $key =~ ^[0-9a-f]{64}$ ]] || return 1
  case "$strategy" in direct|fallback|none) ;; *) return 1 ;; esac
  case "$completeness" in complete|partial) ;; *) return 1 ;; esac
  case "$group_mode" in numeric-supplementary|keep-groups|primary-only|native-inherited) ;; *) return 1 ;; esac
  ct_host_projection_record_scalar "$reason" || return 1
  if [[ $strategy == fallback ]]; then
    (( ${#sources[@]} == ${#targets[@]} && ${#sources[@]} <= CT_HOST_PROJECTION_RECORD_MAX_PAIRS )) || return 1
    for index in "${!sources[@]}"; do
      [[ ${sources[index]} == /* && ${targets[index]} == /* ]] || return 1
    done
  else
    sources=()
    targets=()
  fi
  directory=$(ct_host_projection_cache_directory)
  mkdir -p -- "$directory" || return 1
  temporary=$(mktemp "$directory/.${key}.tmp.XXXXXX") || return 1
  {
    printf '%s\0' "$CT_HOST_PROJECTION_RECORD_VERSION" "$key" "$strategy" "$completeness" "$group_mode" "$reason" "${#sources[@]}"
    for index in "${!sources[@]}"; do
      printf '%s\0' "${sources[index]}" "${targets[index]}"
    done
  } > "$temporary" || {
    rm -f -- "$temporary"
    return 1
  }
  size=$(stat -c '%s' -- "$temporary") || { rm -f -- "$temporary"; return 1; }
  (( size <= CT_HOST_PROJECTION_RECORD_MAX_BYTES )) || { rm -f -- "$temporary"; return 1; }
  mv -f -- "$temporary" "$directory/$key"
}

# Load a bounded selection record. Errors are cache misses and leave no raw
# record contents in globals or diagnostics.
ct_host_projection_cache_read() {
  local key=$1 directory file size pair_count index
  local -a fields=()
  [[ $key =~ ^[0-9a-f]{64}$ ]] || return 1
  directory=$(ct_host_projection_cache_directory)
  file="$directory/$key"
  [[ -f $file ]] || return 1
  size=$(stat -c '%s' -- "$file" 2>/dev/null) || return 1
  (( size <= CT_HOST_PROJECTION_RECORD_MAX_BYTES && size > 0 )) || return 1
  mapfile -d '' -t fields < "$file" || return 1
  (( ${#fields[@]} >= 7 )) || return 1
  [[ ${fields[0]} == "$CT_HOST_PROJECTION_RECORD_VERSION" && ${fields[1]} == "$key" ]] || return 1
  case "${fields[2]}" in direct|fallback|none) ;; *) return 1 ;; esac
  case "${fields[3]}" in complete|partial) ;; *) return 1 ;; esac
  case "${fields[4]}" in numeric-supplementary|keep-groups|primary-only|native-inherited) ;; *) return 1 ;; esac
  ct_host_projection_record_scalar "${fields[5]}" || return 1
  [[ ${fields[6]} =~ ^[0-9]+$ ]] || return 1
  pair_count=${fields[6]}
  (( pair_count <= CT_HOST_PROJECTION_RECORD_MAX_PAIRS )) || return 1
  if [[ ${fields[2]} == fallback ]]; then
    (( ${#fields[@]} == 7 + pair_count * 2 )) || return 1
  else
    (( pair_count == 0 && ${#fields[@]} == 7 )) || return 1
  fi
  CT_HOST_PROJECTION_STRATEGY=${fields[2]}
  CT_HOST_PROJECTION_COMPLETE=${fields[3]}
  CT_HOST_PROJECTION_GROUP_MODE=${fields[4]}
  CT_HOST_PROJECTION_REASON=${fields[5]}
  CT_HOST_PROJECTION_SOURCES=()
  CT_HOST_PROJECTION_TARGETS=()
  CT_HOST_PROJECTION_DESTINATIONS=()
  for ((index = 0; index < pair_count; index++)); do
    [[ ${fields[7 + index * 2]} == /* && ${fields[8 + index * 2]} == /* ]] || return 1
    CT_HOST_PROJECTION_SOURCES+=("${fields[7 + index * 2]}")
    CT_HOST_PROJECTION_TARGETS+=("${fields[8 + index * 2]}")
    if [[ ${fields[7 + index * 2]} == / ]]; then
      CT_HOST_PROJECTION_DESTINATIONS+=(/host)
    else
      CT_HOST_PROJECTION_DESTINATIONS+=("/host${fields[7 + index * 2]}")
    fi
  done
}

# Resolve all cached fallback sources under one aggregate planning deadline.
ct_host_projection_resolve_cached_sources() {
  local timeout_seconds=${CT_HOST_PROJECTION_PLAN_TIMEOUT:-1} output status source
  [[ $timeout_seconds =~ ^[0-9]+([.][0-9]+)?$ && ! $timeout_seconds =~ ^0+([.]0+)?$ ]] || return 2
  output=$(mktemp "${TMPDIR:-/tmp}/ct-host-projection-resolve.XXXXXX") || return 2
  # shellcheck disable=SC2016 # The child must expand its own source argument.
  if timeout --foreground --kill-after=2s "${timeout_seconds}s" bash -c '
    for source; do
      if resolved=$(realpath -e -- "$source" 2>/dev/null); then
        printf "%s\\0" "$resolved"
      else
        # Keep one slot per selected pair so absence is an omission, not a
        # retryable aggregate planning failure.
        printf "\\0"
      fi
    done
  ' bash "${CT_HOST_PROJECTION_SOURCES[@]}" > "$output"; then
    mapfile -d '' -t CT_HOST_PROJECTION_RESOLVED_NOW < "$output"
    status=0
  else
    status=$?
  fi
  rm -f -- "$output"
  return "$status"
}

# Revalidate only stored fallback pairs. Auto omits lost candidates; required
# refuses any loss. It deliberately never enumerates the projection root.
ct_host_projection_validate_fallback() {
  local mode=$1 index source filesystem
  local -a sources=("${CT_HOST_PROJECTION_SOURCES[@]}") targets=("${CT_HOST_PROJECTION_TARGETS[@]}")
  local -a retained_sources=() retained_targets=() retained_destinations=()
  [[ $CT_HOST_PROJECTION_STRATEGY == fallback ]] || return 1
  ct_host_projection_read_mountinfo || return 2
  ct_host_projection_resolve_cached_sources || return 2
  (( ${#CT_HOST_PROJECTION_RESOLVED_NOW[@]} == ${#sources[@]} )) || return 2
  CT_HOST_PROJECTION_COMPLETE=complete
  for index in "${!sources[@]}"; do
    source=${sources[index]}
    filesystem=${CT_HOST_PROJECTION_MOUNT_TYPES[$source]:-}
    if ! ct_host_projection_source_eligible "$source" "$filesystem" \
      || ct_host_projection_has_excluded_descendant "$source" \
      || [[ ${CT_HOST_PROJECTION_RESOLVED_NOW[index]} != "${targets[index]}" ]]; then
      CT_HOST_PROJECTION_COMPLETE=partial
      continue
    fi
    retained_sources+=("$source")
    retained_targets+=("${targets[index]}")
    if [[ $source == / ]]; then
      retained_destinations+=(/host)
    else
      retained_destinations+=("/host$source")
    fi
  done
  CT_HOST_PROJECTION_SOURCES=("${retained_sources[@]}")
  CT_HOST_PROJECTION_TARGETS=("${retained_targets[@]}")
  CT_HOST_PROJECTION_DESTINATIONS=("${retained_destinations[@]}")
  [[ $mode == auto || $CT_HOST_PROJECTION_COMPLETE == complete ]]
}

# Acquire one bounded, per-selection cold-path lock. Callers own the matching
# release and apply auto/required policy when this returns failure.
ct_host_projection_acquire_lock() {
  local key=$1 timeout_seconds=${CT_HOST_PROJECTION_LOCK_TIMEOUT:-8} directory
  if [[ -n ${CT_HOST_PROJECTION_COLD_DEADLINE:-} && -z ${CT_HOST_PROJECTION_LOCK_TIMEOUT:-} ]]; then
    timeout_seconds=$((CT_HOST_PROJECTION_COLD_DEADLINE - SECONDS))
  fi
  [[ $key =~ ^[0-9a-f]{64}$ ]] || return 1
  [[ $timeout_seconds =~ ^[0-9]+([.][0-9]+)?$ && ! $timeout_seconds =~ ^0+([.]0+)?$ ]] || return 1
  directory=$(ct_host_projection_cache_directory)
  mkdir -p -- "$directory/.locks" || return 1
  exec {CT_HOST_PROJECTION_LOCK_FD}> "$directory/.locks/$key.lock" || return 1
  if ! flock -w "$timeout_seconds" "$CT_HOST_PROJECTION_LOCK_FD"; then
    exec {CT_HOST_PROJECTION_LOCK_FD}>&-
    unset CT_HOST_PROJECTION_LOCK_FD
    return 1
  fi
}

# Release a lock obtained by ct_host_projection_acquire_lock.
ct_host_projection_release_lock() {
  [[ -n ${CT_HOST_PROJECTION_LOCK_FD:-} ]] || return 0
  flock -u "$CT_HOST_PROJECTION_LOCK_FD" || return 1
  exec {CT_HOST_PROJECTION_LOCK_FD}>&-
  unset CT_HOST_PROJECTION_LOCK_FD
}

# Consume a current selection or serialize one caller-supplied cold proof. The
# proof callback sets the selection globals and returns only after its cleanup
# is confirmed; a failed refresh consequently leaves the old record intact.
ct_host_projection_select() {
  local key=$1 refresh=$2 selector=$3
  shift 3
  # shellcheck disable=SC2034 # This is the public result for U2 callers.
  CT_HOST_PROJECTION_CACHE_HIT=0
  if [[ $refresh != 1 ]] && ct_host_projection_cache_read "$key"; then
    # shellcheck disable=SC2034 # This is the public result for U2 callers.
    CT_HOST_PROJECTION_CACHE_HIT=1
    return 0
  fi
  ct_host_projection_acquire_lock "$key" || return 1
  if [[ $refresh != 1 ]] && ct_host_projection_cache_read "$key"; then
    # shellcheck disable=SC2034 # This is the public result for U2 callers.
    CT_HOST_PROJECTION_CACHE_HIT=1
    ct_host_projection_release_lock
    return 0
  fi
  if "$selector" "$@"; then
    ct_host_projection_cache_write "$key" "$CT_HOST_PROJECTION_STRATEGY" \
      "$CT_HOST_PROJECTION_COMPLETE" "$CT_HOST_PROJECTION_GROUP_MODE" "$CT_HOST_PROJECTION_REASON" || true
    ct_host_projection_release_lock
    return 0
  fi
  ct_host_projection_release_lock
  return 1
}

# Render selected generated binds into backend argv fragments without exposing
# source paths through word splitting or a serialized command line.
ct_host_projection_render_mounts() {
  local backend=$1 index source destination
  CT_HOST_PROJECTION_MOUNT_ARGS=()
  # shellcheck disable=SC2034 # This typed result is consumed by launch integration.
  CT_HOST_PROJECTION_RENDER_FAILURE=
  for index in "${!CT_HOST_PROJECTION_SOURCES[@]}"; do
    source=${CT_HOST_PROJECTION_SOURCES[index]}
    destination=${CT_HOST_PROJECTION_DESTINATIONS[index]}
    [[ $source == /* && $destination == /host/* || $destination == /host ]] || return 1
    case "$backend" in
      docker)
        CT_HOST_PROJECTION_MOUNT_ARGS+=(--mount "type=bind,source=$source,target=$destination,bind-recursive=disabled")
        ;;
      podman)
        CT_HOST_PROJECTION_MOUNT_ARGS+=(--mount "type=bind,source=$source,target=$destination,bind-nonrecursive")
        ;;
      singularity|apptainer)
        [[ $source != *,* && $destination != *,* ]] || {
          # shellcheck disable=SC2034 # This typed result is consumed by launch integration.
          CT_HOST_PROJECTION_RENDER_FAILURE=unrepresentable-path
          return 1
        }
        CT_HOST_PROJECTION_MOUNT_ARGS+=(--mount "type=bind,src=$source,dst=$destination")
        ;;
      *) return 1 ;;
    esac
  done
}

# Return 0 only for the default local Docker/Podman client endpoint. Explicit
# TCP, SSH, machine, and non-default socket selectors are intentionally not a
# host projection capability because their daemon host is not this host.
ct_host_projection_local_endpoint() {
  local backend=$1 endpoint
  case "$backend" in
    docker) endpoint=${DOCKER_HOST:-} ;;
    podman) endpoint=${CONTAINER_HOST:-${DOCKER_HOST:-}} ;;
    singularity|apptainer) return 0 ;;
    *) return 1 ;;
  esac
  [[ -z $endpoint || $endpoint == unix://* || $endpoint == unix:* ]]
}

# Set the least-privilege group realization used when a probe has not
# conclusively reported broader group support. The result is also used by U3
# when it builds a persistent profile, including launches without host binds.
ct_host_projection_apply_group_mode() {
  local backend=$1 group
  CT_HOST_PROJECTION_GROUP_ARGS=()
  case "${CT_HOST_PROJECTION_GROUP_MODE:-}" in
    numeric-supplementary)
      [[ $backend == docker ]] || return 1
      while IFS= read -r group; do
        [[ $group == "$GROUP_ID" ]] || CT_HOST_PROJECTION_GROUP_ARGS+=(--group-add "$group")
      done < <(id -G | tr ' ' '\n' | LC_ALL=C sort -n -u)
      ;;
    keep-groups)
      [[ $backend == podman ]] || return 1
      CT_HOST_PROJECTION_GROUP_ARGS+=(--group-add keep-groups)
      ;;
    primary-only)
      [[ $backend == docker || $backend == podman ]] || return 1
      ;;
    native-inherited)
      [[ $backend == singularity || $backend == apptainer ]] || return 1
      ;;
    *) return 1 ;;
  esac
}

# Reduce a generated plan against already assembled mounts. Existing equivalent
# /host binds retain their ordering and meaning; conflicts make this generated
# candidate incomplete rather than overwriting a caller-provided mount.
ct_host_projection_resolve_mount_conflicts() {
  local index candidate source destination mount_index mount descriptor existing_source
  local -a sources=() targets=() destinations=()
  for index in "${!CT_HOST_PROJECTION_SOURCES[@]}"; do
    source=${CT_HOST_PROJECTION_SOURCES[index]}
    destination=${CT_HOST_PROJECTION_DESTINATIONS[index]}
    candidate=keep
    for ((mount_index = 0; mount_index < ${#MOUNT_ARGS[@]}; mount_index++)); do
      mount=${MOUNT_ARGS[mount_index]}
      [[ $mount == --mount || $mount == --bind ]] || continue
      descriptor=${MOUNT_ARGS[mount_index + 1]:-}
      if [[ $mount == --mount ]]; then
        [[ $descriptor == *"target=$destination"* || $descriptor == *"dst=$destination"* ]] || continue
        if [[ $descriptor == *"source=$source,"* || $descriptor == *"src=$source,"* ]]; then
          candidate=equivalent
        else
          candidate=conflict
        fi
      else
        existing_source=${descriptor%%:*}
        [[ $descriptor == *":$destination" || $descriptor == *":$destination:"* ]] || continue
        [[ $existing_source == "$source" ]] && candidate=equivalent || candidate=conflict
      fi
      [[ $candidate == conflict ]] && break
    done
    case "$candidate" in
      keep)
        sources+=("$source")
        targets+=("${CT_HOST_PROJECTION_TARGETS[index]}")
        destinations+=("$destination")
        ;;
      equivalent) ;;
      conflict)
        CT_HOST_PROJECTION_COMPLETE=partial
        ;;
    esac
  done
  CT_HOST_PROJECTION_SOURCES=("${sources[@]}")
  CT_HOST_PROJECTION_TARGETS=("${targets[@]}")
  CT_HOST_PROJECTION_DESTINATIONS=("${destinations[@]}")
}

# Record a conservative no-projection result without leaking probe output or
# runtime arguments. A clean `none` is reusable by warm auto and required calls.
ct_host_projection_set_none() {
  local backend=$1 reason=$2
  CT_HOST_PROJECTION_SOURCES=()
  CT_HOST_PROJECTION_TARGETS=()
  CT_HOST_PROJECTION_DESTINATIONS=()
  CT_HOST_PROJECTION_STRATEGY=none
  CT_HOST_PROJECTION_COMPLETE=complete
  case "$backend" in
    docker|podman) CT_HOST_PROJECTION_GROUP_MODE=primary-only ;;
    singularity|apptainer) CT_HOST_PROJECTION_GROUP_MODE=native-inherited ;;
    *) return 1 ;;
  esac
  CT_HOST_PROJECTION_REASON=$reason
}

# Execute one aggregate capability proof with no payload environment, bootstrap,
# or inherited Singularity bind variables. Its only accepted output is the
# closed group marker; all other output is discarded.
ct_host_projection_probe() {
  local backend=$1 image=$2 strategy=$3 requested_group_mode=$4
  local output status timeout_seconds remaining probe_name probe_id cleanup_status interrupted=0 group group_marker
  local -a probe=() create=() group_args=() expected_groups=()
  ct_host_projection_render_mounts "$backend" || return 2
  timeout_seconds=${CT_HOST_PROJECTION_PROBE_TIMEOUT:-8}
  if [[ -n ${CT_HOST_PROJECTION_COLD_DEADLINE:-} && -z ${CT_HOST_PROJECTION_PROBE_TIMEOUT:-} ]]; then
    timeout_seconds=$((CT_HOST_PROJECTION_COLD_DEADLINE - SECONDS))
  fi
  [[ $timeout_seconds =~ ^[0-9]+([.][0-9]+)?$ && ! $timeout_seconds =~ ^0+([.]0+)?$ ]] || return 2
  while IFS= read -r group; do
    [[ $group == "$GROUP_ID" ]] || expected_groups+=("$group")
  done < <(id -G | tr ' ' '\n' | LC_ALL=C sort -n -u)
  case "$requested_group_mode" in
    numeric-supplementary)
      [[ $backend == docker ]] || return 2
      group_marker=numeric
      for group in "${expected_groups[@]}"; do group_args+=(--group-add "$group"); done
      ;;
    keep-groups)
      [[ $backend == podman ]] || return 2
      group_marker=keep
      group_args+=(--group-add keep-groups)
      ;;
    primary-only)
      [[ $backend == docker || $backend == podman ]] || return 2
      group_marker=none
      expected_groups=()
      ;;
    native-inherited)
      [[ $backend == singularity || $backend == apptainer ]] || return 2
      ;;
    *) return 2 ;;
  esac
  case "$backend" in
    docker|podman)
      probe_name="ct-host-projection-${EUID}-$$-${RANDOM}-${SECONDS}"
      create=("${TOOL[@]}" create --name "$probe_name"
        --label "container-tools.host-projection.probe=$probe_name")
      [[ $backend == podman ]] && create+=(--userns=keep-id)
      # shellcheck disable=SC2016 # The probe shell expands its own positional inputs.
      create+=(--user "$USER_ID:$GROUP_ID" "${group_args[@]}"
        "${CT_HOST_PROJECTION_MOUNT_ARGS[@]}" "$image" /bin/sh -c '
          test -d /host || exit 20
          expected=$1
          shift
          actual=" $(id -G) "
          for group; do
            case "$actual" in *" $group "*) ;; *) exit 21 ;; esac
          done
          printf "ct-host-projection-group=%s\n" "$expected"
        ' sh "$group_marker" "${expected_groups[@]}")
      ;;
    singularity|apptainer)
      probe=("${TOOL[@]}" exec "${CT_HOST_PROJECTION_MOUNT_ARGS[@]}" "$image" /bin/sh -c '
        test -d /host || exit 20
        printf "ct-host-projection-group=native\n"
      ')
      ;;
    *) return 2 ;;
  esac
  output=$(mktemp "${TMPDIR:-/tmp}/ct-host-projection-probe.XXXXXX") || return 2
  if [[ $backend == docker || $backend == podman ]]; then
    trap 'interrupted=1' HUP INT TERM
    if probe_id=$(env -i PATH="$PATH" HOME="${HOME:-/}" \
      timeout --foreground --kill-after=2s "${timeout_seconds}s" "${create[@]}" 2>/dev/null); then
      if [[ ! $probe_id =~ ^[A-Za-z0-9._-]+$ ]]; then
        status=2
      else
        remaining=${CT_HOST_PROJECTION_COLD_DEADLINE:-0}
        if (( remaining > 0 )); then
          remaining=$((remaining - SECONDS))
        else
          remaining=$timeout_seconds
        fi
        if (( remaining <= 0 )); then
          status=124
        elif env -i PATH="$PATH" HOME="${HOME:-/}" \
          timeout --foreground --kill-after=2s "${remaining}s" \
          "${TOOL[@]}" start --attach "$probe_id" > "$output" 2>/dev/null; then
          status=0
        else
          status=$?
        fi
        if env -i PATH="$PATH" HOME="${HOME:-/}" \
          timeout --foreground --kill-after=1s 2s "${TOOL[@]}" rm --force "$probe_id" >/dev/null 2>&1; then
          cleanup_status=0
        else
          cleanup_status=$?
        fi
        (( cleanup_status == 0 )) || status=2
      fi
    else
      status=$?
    fi
    trap - HUP INT TERM
    (( interrupted == 0 )) || status=2
  else
    if env -i PATH="$PATH" HOME="${HOME:-/}" \
      timeout --foreground --kill-after=2s "${timeout_seconds}s" "${probe[@]}" > "$output" 2>/dev/null; then
      status=0
    else
      status=$?
    fi
  fi
  if (( status == 0 )); then
    case "$(<"$output")" in
      ct-host-projection-group=numeric) CT_HOST_PROJECTION_GROUP_MODE=numeric-supplementary ;;
      ct-host-projection-group=keep) CT_HOST_PROJECTION_GROUP_MODE=keep-groups ;;
      ct-host-projection-group=native) CT_HOST_PROJECTION_GROUP_MODE=native-inherited ;;
      *)
        # Inconclusive or malformed group output cannot grant supplementary access.
        case "$backend" in docker|podman) CT_HOST_PROJECTION_GROUP_MODE=primary-only ;; *) CT_HOST_PROJECTION_GROUP_MODE=native-inherited ;; esac
        ;;
    esac
  fi
  rm -f -- "$output"
  return "$status"
}

# Make the minimum mode-independent cold selection. Docker and Podman attempt
# direct then fallback; native image runtimes have no documented non-recursive
# root bind and prove fallback only. Probe timeout or setup uncertainty is not
# a conclusive `none` and is never published.
ct_host_projection_cold_select() {
  local backend=$1 image=$2 probe_status
  CT_HOST_PROJECTION_SELECTION_FAILURE=
  case "$backend" in
    docker|podman)
      ct_host_projection_build_direct || return 1
      if [[ $backend == docker ]]; then
        CT_HOST_PROJECTION_GROUP_MODE=numeric-supplementary
      else
        CT_HOST_PROJECTION_GROUP_MODE=keep-groups
      fi
      if ct_host_projection_probe "$backend" "$image" direct "$CT_HOST_PROJECTION_GROUP_MODE"; then
        CT_HOST_PROJECTION_REASON=proven
        return 0
      else
        probe_status=$?
      fi
      if (( probe_status == 124 || probe_status == 137 || probe_status == 2 )); then
        CT_HOST_PROJECTION_SELECTION_FAILURE=terminal
        return 1
      fi
      CT_HOST_PROJECTION_GROUP_MODE=primary-only
      ;;
    singularity|apptainer) CT_HOST_PROJECTION_GROUP_MODE=native-inherited ;;
    *) return 1 ;;
  esac
  ct_host_projection_build_fallback || return 1
  if ct_host_projection_probe "$backend" "$image" fallback "$CT_HOST_PROJECTION_GROUP_MODE"; then
    CT_HOST_PROJECTION_REASON=proven
    return 0
  else
    probe_status=$?
  fi
  if (( probe_status == 124 || probe_status == 137 || probe_status == 2 )); then
    CT_HOST_PROJECTION_SELECTION_FAILURE=terminal
    return 1
  fi
  ct_host_projection_set_none "$backend" unavailable
}

# Consume or establish a selection, render its currently usable mounts, and
# prepend them before existing mounts. Auto degrades to the pre-existing launch
# while required refuses before the payload; neither path retries a payload.
ct_host_projection_prepare_foreground() {
  local backend=${TOOL[0]} image=${PAYLOAD_ARGS[0]:-} requested_group_mode cache_key selected=0 cold_timeout
  CT_HOST_PROJECTION_GENERATED_MOUNT_ARGS=()
  # shellcheck disable=SC2034 # Foreground launchers consume this shared result.
  CT_HOST_PROJECTION_RUNTIME_LABEL_ARGS=()
  if [[ -n ${CT_HOST_PROJECTION_RUNTIME_LABEL:-} && ( $backend == docker || $backend == podman ) ]]; then
    [[ $CT_HOST_PROJECTION_RUNTIME_LABEL =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || return 1
    CT_HOST_PROJECTION_RUNTIME_LABEL_ARGS=(--label "container-tools.host-projection-runtime.run=$CT_HOST_PROJECTION_RUNTIME_LABEL")
  fi
  [[ ${CT_HOST_ROOT:-auto} == auto || ${CT_HOST_ROOT:-auto} == required ]] || return 1
  [[ -n $image ]] || {
    [[ $CT_HOST_ROOT == auto ]] && return 0
    printf '%s\n' 'Error: --ct-host-root required needs a container image' >&2
    return 1
  }
  case "$backend" in
    docker) requested_group_mode=numeric-supplementary ;;
    podman) requested_group_mode=keep-groups ;;
    singularity|apptainer) requested_group_mode=native-inherited ;;
    *) return 1 ;;
  esac
  ct_host_projection_set_none "$backend" unavailable
  if ! ct_host_projection_local_endpoint "$backend"; then
    [[ $CT_HOST_ROOT == auto ]] && {
      printf '%s\n' 'WARNING: host projection unavailable for an explicit remote runtime endpoint' >&2
      ct_host_projection_apply_group_mode "$backend"
      return 0
    }
    printf '%s\n' 'Error: host projection is unavailable for an explicit remote runtime endpoint' >&2
    return 1
  fi
  ct_host_projection_selection_key "$backend" "$image" "${TOOL[*]:1}" "$requested_group_mode" || return 1
  cache_key=$CT_HOST_PROJECTION_SELECTION_KEY
  cold_timeout=${CT_HOST_PROJECTION_COLD_TIMEOUT:-8}
  [[ $cold_timeout =~ ^[0-9]+$ && $cold_timeout != 0 ]] || return 1
  # The lock, planning, and one-or-two aggregate probes share this foreground
  # selection budget. Cleanup belongs to each runtime's bounded `--rm` path.
  CT_HOST_PROJECTION_COLD_DEADLINE=$((SECONDS + cold_timeout))
  if ct_host_projection_select "$cache_key" "${CT_HOST_ROOT_REFRESH:-0}" \
    ct_host_projection_cold_select "$backend" "$image"; then
    selected=1
  fi
  unset CT_HOST_PROJECTION_COLD_DEADLINE
  if (( selected )) && [[ $CT_HOST_PROJECTION_STRATEGY == direct ]]; then
    ct_host_projection_build_direct || selected=0
  elif (( selected )) && [[ $CT_HOST_PROJECTION_STRATEGY == fallback ]]; then
    ct_host_projection_validate_fallback auto || selected=0
  fi
  if (( ! selected )); then
    [[ ${CT_HOST_PROJECTION_SELECTION_FAILURE:-} != terminal ]] || {
      printf '%s\n' 'Error: host projection proof did not terminate with confirmed cleanup' >&2
      return 1
    }
    ct_host_projection_set_none "$backend" unavailable
  fi
  if [[ $CT_HOST_PROJECTION_STRATEGY != none ]]; then
    ct_host_projection_resolve_mount_conflicts
    ct_host_projection_render_mounts "$backend" || {
      CT_HOST_PROJECTION_COMPLETE=partial
      CT_HOST_PROJECTION_MOUNT_ARGS=()
    }
  else
    CT_HOST_PROJECTION_MOUNT_ARGS=()
  fi
  ct_host_projection_apply_group_mode "$backend" || return 1
  if [[ $CT_HOST_ROOT == required && ( $CT_HOST_PROJECTION_STRATEGY == none || $CT_HOST_PROJECTION_COMPLETE != complete ) ]]; then
    printf '%s\n' 'Error: complete host projection is required but unavailable' >&2
    return 1
  fi
  if [[ $CT_HOST_ROOT == auto && ( $CT_HOST_PROJECTION_STRATEGY == none || $CT_HOST_PROJECTION_COMPLETE != complete ) ]]; then
    printf '%s\n' 'WARNING: host projection is incomplete; dispatching the existing foreground launch' >&2
  fi
  CT_HOST_PROJECTION_GENERATED_MOUNT_ARGS=("${CT_HOST_PROJECTION_MOUNT_ARGS[@]}")
  MOUNT_ARGS=("${CT_HOST_PROJECTION_GENERATED_MOUNT_ARGS[@]}" "${MOUNT_ARGS[@]}")
}

determine_container_tool() {
  TOOL=()
  case "$1" in
    --docker)
      TOOL+=(docker)
      ;;
    --podman)
      TOOL+=(podman)
      ;;
    --singularity)
      TOOL+=(singularity)
      if [[ -n ${CT_SINGULARITY_ARGS:=} ]]; then
        TOOL+=("$CT_SINGULARITY_ARGS")
      fi
      ;;
    --apptainer)
      TOOL+=(apptainer)
      if [[ -n ${CT_SINGULARITY_ARGS:=} ]]; then
        TOOL+=("$CT_SINGULARITY_ARGS")
      fi
      ;;
    *) ;;
  esac

  if [[ ${#TOOL[@]} -eq 0 ]]; then
    echo "Error: Must specify one of --docker, --podman, --singularity, or --apptainer" >&2
    return 1
  fi
}

determine_tool_mode() {
  MODE=""
  case "$1" in
    exec) MODE="exec" ;;
    shell) MODE="exec" ;;
    *) ;;
  esac

  if [[ -z $MODE ]]; then
    echo "Error: Mode must be one of exec, shell" >&2
    return 1
  fi
}

valid_bind_src() {
  local src=$1

  [[ -n $src && $src = /* ]] || return 1
  timeout 2s stat -L -- "$src" >/dev/null 2>&1 || return 1
}

bind_src_error() {
  local src=$1

  if [[ -z $src ]]; then
    echo "empty source path"
  elif [[ $src != /* ]]; then
    echo "source path is not absolute"
  else
    timeout 2s stat -L -- "$src" >/dev/null 2> >(head -n 1) || true
  fi
}

warn_dropped_mount() {
  local mnt=$1
  local reason=$2

  {
    echo "WARNING: dropping container bind mount:"
    echo "  requested: $mnt"
    echo "  reason:    $reason"
    echo "  result:    this path will NOT be visible inside the container"
  } >&2
}

append_mount_arg() {
  local host=$1
  local container=$2

  case "${TOOL[0]}" in
    docker|podman)
      MOUNT_ARGS+=(--mount "type=bind,source=${host},target=${container}")
      ;;
    singularity|apptainer)
      MOUNT_ARGS+=(--bind "${host}:${container}")
      ;;
  esac
}

append_bootstrap_mount_arg() {
  case "${TOOL[0]}" in
    docker|podman)
      MOUNT_ARGS+=(--mount "type=bind,source=${CT_BOOTSTRAP},target=${CT_BOOTSTRAP_CONTAINER},readonly")
      ;;
    singularity|apptainer)
      MOUNT_ARGS+=(--bind "${CT_BOOTSTRAP}:${CT_BOOTSTRAP_CONTAINER}:ro")
      ;;
  esac
}

parse_explicit_mount_args() {
  EXPLICIT_MOUNTS=()
  CT_ENVS=()
  CT_BOOTSTRAP=""
  CT_BOOTSTRAP_CONTAINER=/.container-tools-bootstrap
  CT_CONTAINER_SHELL=/bin/sh
  CT_HOST_ROOT=auto
  CT_HOST_ROOT_REFRESH=0
  CT_PAYLOAD_DELIMITED=0
  CT_DELIMITER_POSITION=0
  local remaining=()
  local env_pattern='^[A-Za-z_][A-Za-z0-9_]*=[A-Za-z0-9_./:@%+ =-]*$'

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --ct-bind)
        if [[ $# -lt 2 ]]; then
          echo "Error: --ct-bind requires HOST_PATH:CONTAINER_PATH" >&2
          return 1
        fi

        local mnt=$2
        local host=${mnt%%:*}
        local container=${mnt#*:}
        if [[ $host == "$mnt" || -z $container || $container != /* ]]; then
          echo "Error: invalid --ct-bind '$mnt' (expected HOST_PATH:CONTAINER_PATH)" >&2
          return 1
        fi
        if ! valid_bind_src "$host"; then
          local reason
          reason=$(bind_src_error "$host")
          echo "Error: invalid explicit container bind '$mnt': ${reason:-source path is unavailable}" >&2
          return 1
        fi

        EXPLICIT_MOUNTS+=("$mnt")
        shift 2
        ;;
      --ct-env)
        if [[ $# -lt 2 ]]; then
          echo "Error: --ct-env requires NAME=VALUE" >&2
          return 1
        fi
        if [[ ! $2 =~ $env_pattern ]]; then
          echo "Error: invalid --ct-env '$2' (use NAME=VALUE with shell-literal path/text characters)" >&2
          return 1
        fi
        CT_ENVS+=("$2")
        shift 2
        ;;
      --ct-bootstrap)
        if [[ $# -lt 2 ]]; then
          echo "Error: --ct-bootstrap requires an executable host path" >&2
          return 1
        fi
        if [[ -n $CT_BOOTSTRAP ]]; then
          echo "Error: --ct-bootstrap may only be specified once" >&2
          return 1
        fi
        if [[ $2 != /* || $2 == *:* || $2 == *,* || $2 == *$'\n'* || ! -f $2 || ! -r $2 || ! -x $2 ]]; then
          echo "Error: invalid --ct-bootstrap '$2' (expected an absolute, readable executable without ':', comma, or newline)" >&2
          return 1
        fi
        CT_BOOTSTRAP=$2
        shift 2
        ;;
      --ct-container-shell)
        if [[ $# -lt 2 || $2 != /* ]]; then
          echo "Error: --ct-container-shell requires an absolute container path" >&2
          return 1
        fi
        CT_CONTAINER_SHELL=$2
        shift 2
        ;;
      --ct-host-root)
        if [[ $# -lt 2 || ( $2 != auto && $2 != required ) ]]; then
          echo "Error: --ct-host-root requires auto or required" >&2
          return 1
        fi
        CT_HOST_ROOT=$2
        shift 2
        ;;
      --ct-host-root-refresh)
        CT_HOST_ROOT_REFRESH=1
        shift
        ;;
      --)
        CT_PAYLOAD_DELIMITED=1
        CT_DELIMITER_POSITION=${#remaining[@]}
        shift
        remaining+=("$@")
        break
        ;;
      *)
        remaining+=("$1")
        shift
        ;;
    esac
  done

  if [[ $CT_PAYLOAD_DELIMITED -eq 1 && -z $CT_BOOTSTRAP ]]; then
    remaining=("${remaining[@]:0:CT_DELIMITER_POSITION}" -- "${remaining[@]:CT_DELIMITER_POSITION}")
  fi
  ARGS=("${remaining[@]}")
}

build_mount_args() {
  MOUNT_ARGS=()

  CT_MOUNT_CFG=${CT_MOUNT_CFG:=${HOME}/.config/ct_mount.conf}
  if [[ -e $CT_MOUNT_CFG ]]; then
    MOUNT_DETECTOR_ARGS=$("$script_dir/ct_args.sh" "$CT_MOUNT_CFG" ${MOUNT_DETECTOR_ARGS:=})
  fi

  while IFS= read -r mnt; do
    [[ -n $mnt ]] || continue

    local host
    local container
    if [[ $mnt == *:* ]]; then
      host=${mnt%%:*}
      container=${mnt#*:}
    else
      host=$mnt
      container=$mnt
    fi

    if ! valid_bind_src "$host"; then
      local reason
      reason=$(bind_src_error "$host")
      [[ -n $reason ]] || reason="source path is unavailable or stat timed out"
      warn_dropped_mount "$mnt" "$reason"
      continue
    fi

    append_mount_arg "$host" "$container"
  done < <("$script_dir/ct_mount_detector.sh" ${MOUNT_DETECTOR_ARGS:=})

  for mnt in "${EXPLICIT_MOUNTS[@]}"; do
    local host=${mnt%%:*}
    local container=${mnt#*:}
    append_mount_arg "$host" "$container"
  done

  if [[ -n $CT_BOOTSTRAP ]]; then
    append_bootstrap_mount_arg
  fi
}

build_env_args() {
  if [[ ${TOOL[0]} == docker || ${TOOL[0]} == podman ]]; then
    ENV_ARGS=(--env-file <(env))
  else
    ENV_ARGS=()
  fi

  for assignment in "${CT_ENVS[@]}"; do
    ENV_ARGS+=(--env "$assignment")
  done
}

build_payload_args() {
  local mode=$1
  PAYLOAD_ARGS=("${ARGS[@]}")

  [[ -n $CT_BOOTSTRAP ]] || return 0
  if [[ $CT_PAYLOAD_DELIMITED -ne 1 ]]; then
    echo "Error: --ct-bootstrap requires '--' before the container image" >&2
    return 1
  fi

  case "$mode" in
    exec)
      if [[ ${#ARGS[@]} -lt 2 ]]; then
        echo "Error: bootstrap exec requires a container image and command" >&2
        return 1
      fi
      PAYLOAD_ARGS=("${ARGS[0]}" "$CT_BOOTSTRAP_CONTAINER" "${ARGS[@]:1}")
      ;;
    shell)
      if [[ ${#ARGS[@]} -ne 1 ]]; then
        echo "Error: bootstrap shell requires exactly one container image" >&2
        return 1
      fi
      PAYLOAD_ARGS=("${ARGS[0]}" "$CT_BOOTSTRAP_CONTAINER" "$CT_CONTAINER_SHELL" -i)
      ;;
    *)
      echo "Error: unsupported bootstrap mode '$mode'" >&2
      return 1
      ;;
  esac
}

run_cmd() {
  if [[ -n ${CT_DRY_RUN:=} ]]; then
    printf '%q ' "${CMD[@]}"
    echo
  else
    unset SINGULARITY_BIND SINGULARITY_BINDPATH
    unset APPTAINER_BIND APPTAINER_BINDPATH
    exec "${CMD[@]}"
  fi
}

launcher_preamble() {
  if [[ $# -eq 0 ]]; then
    echo "Error: container backend is required" >&2
    return 1
  fi
  determine_container_tool "$1" || return 1
  case "${TOOL[0]}" in
    singularity|apptainer) configure_runtime_storage "${TOOL[0]}" || return 1 ;;
  esac
  shift

  parse_explicit_mount_args "$@" || return 1

  USER_ID=$(id -u)
  GROUP_ID=$(id -g)
  PWD_DIR=$PWD

  build_mount_args
  build_env_args
}
