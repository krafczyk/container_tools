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
