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
  shift

  parse_explicit_mount_args "$@" || return 1

  USER_ID=$(id -u)
  GROUP_ID=$(id -g)
  PWD_DIR=$PWD

  build_mount_args
  build_env_args
}
