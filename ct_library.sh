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
      if [ -n "${CT_SINGULARITY_ARGS:=}" ]; then
        TOOL+=("${CT_SINGULARITY_ARGS}")
      fi;
      ;;
    --apptainer)
      TOOL+=(apptainer)
      if [ -n "${CT_SINGULARITY_ARGS:=}" ]; then
        TOOL+=("${CT_SINGULARITY_ARGS}")
      fi;
      ;;
    *) ;;
  esac

  if [ -z "$TOOL" ]; then
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

  if [ -z "$MODE" ]; then
    echo "Error: Mode musbe be one of exec, shell" >&2
    return 1
  fi;
}

valid_bind_src() {
  local src="$1"

  [[ -n "$src" ]] || return 1
  [[ "$src" = /* ]] || return 1

  timeout 2s stat -L -- "$src" >/dev/null 2>&1 || return 1

  return 0
}

bind_src_error() {
  local src="$1"

  if [[ -z "$src" ]]; then
    echo "empty source path"
  elif [[ "$src" != /* ]]; then
    echo "source path is not absolute"
  else
    # Capture actual stat error, e.g. "No such device" / "Input/output error"
    timeout 2s stat -L -- "$src" >/dev/null 2> >(head -n 1) || true
  fi
}

warn_dropped_mount() {
  local mnt="$1"
  local reason="$2"

  {
    echo "WARNING: dropping container bind mount:"
    echo "  requested: $mnt"
    echo "  reason:    $reason"
    echo "  result:    this path will NOT be visible inside the container"
  } >&2
}

append_mount_arg() {
  local host="$1"
  local container="$2"

  case "${TOOL[0]}" in
    docker|podman)
      MOUNT_ARGS+=(--mount "type=bind,source=${host},target=${container}")
      ;;
    singularity|apptainer)
      MOUNT_ARGS+=(--bind "${host}:${container}")
      ;;
  esac
}

parse_explicit_mount_args() {
  EXPLICIT_MOUNTS=()
  local remaining=()

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --ct-bind)
        if [[ $# -lt 2 ]]; then
          echo "Error: --ct-bind requires HOST_PATH:CONTAINER_PATH" >&2
          return 1
        fi

        local mnt="$2"
        local host="${mnt%%:*}"
        local container="${mnt#*:}"
        if [[ "$host" == "$mnt" || -z "$container" || "$container" != /* ]]; then
          echo "Error: invalid --ct-bind '$mnt' (expected HOST_PATH:CONTAINER_PATH)" >&2
          return 1
        fi
        if ! valid_bind_src "$host"; then
          local reason
          reason="$(bind_src_error "$host")"
          echo "Error: invalid explicit container bind '$mnt': ${reason:-source path is unavailable}" >&2
          return 1
        fi

        EXPLICIT_MOUNTS+=("$mnt")
        shift 2
        ;;
      *)
        remaining+=("$1")
        shift
        ;;
    esac
  done

  ARGS=("${remaining[@]}")
}

build_mount_args() {
  MOUNT_ARGS=()

  CT_MOUNT_CFG="${CT_MOUNT_CFG:=${HOME}/.config/ct_mount.conf}"
  if [ -e "$CT_MOUNT_CFG" ]; then
    MOUNT_DETECTOR_ARGS=$("$script_dir/ct_args.sh" "$CT_MOUNT_CFG" ${MOUNT_DETECTOR_ARGS:=})
  fi

  while IFS= read -r mnt; do
    [[ -n "$mnt" ]] || continue

    if [[ "$mnt" == *:* ]]; then
      host="${mnt%%:*}"
      container="${mnt#*:}"
    else
      host="$mnt"
      container="$mnt"
    fi

    if ! valid_bind_src "$host"; then
      reason="$(bind_src_error "$host")"
      [[ -n "$reason" ]] || reason="source path is unavailable or stat timed out"
      warn_dropped_mount "$mnt" "$reason"
      continue
    fi

    append_mount_arg "$host" "$container"
  done < <("$script_dir/ct_mount_detector.sh" ${MOUNT_DETECTOR_ARGS:=})

  # Explicit mounts are part of the launch contract.  They were validated by
  # parse_explicit_mount_args(), unlike automatically detected mounts, which
  # remain best-effort.
  for mnt in "${EXPLICIT_MOUNTS[@]}"; do
    host="${mnt%%:*}"
    container="${mnt#*:}"
    append_mount_arg "$host" "$container"
  done
}

build_env_args() {
  # Environment: pass the entire host environment.
  if [[ "$TOOL" == "docker" || "$TOOL" == "podman" ]]; then
    # Process substitution returns a filename that lists all env variables.
    ENV_ARGS=(--env-file <(env))
  elif [[ "$TOOL" == "singularity" || "$TOOL" == "apptainer" ]]; then
    #ENV_ARGS=("-E")
    ENV_ARGS=()
fi
}

run_cmd() {
  if [ -n "${CT_DRY_RUN:=}" ]; then
    printf '%q ' "${CMD[@]}"
    echo
  else
    unset SINGULARITY_BIND SINGULARITY_BINDPATH
    unset APPTAINER_BIND APPTAINER_BINDPATH
    exec "${CMD[@]}"
  fi
}

launcher_preamble() {
  determine_container_tool $1 || exit 1

  shift;

  # Rebuild the argument list without the tool and consume container-tool
  # options before forwarding the remainder to the selected backend.
  parse_explicit_mount_args "$@" || return 1

  # User details and working directory.
  USER_ID="$(id -u)"
  GROUP_ID="$(id -g)"
  PWD_DIR="$PWD"

  build_mount_args

  build_env_args
}
