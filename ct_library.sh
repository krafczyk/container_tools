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

build_mount_args() {
  # Initialize mount args array
  MOUNT_ARGS=()

  # Load mount arguments from config file and environment
  CT_MOUNT_CFG="${CT_MOUNT_CFG:=${HOME}/.config/ct_mount.conf}"
  if [ -e ${CT_MOUNT_CFG} ]; then
    MOUNT_DETECTOR_ARGS=$($script_dir/ct_args.sh $CT_MOUNT_CFG ${MOUNT_DETECTOR_ARGS:=})
  fi;

  while IFS= read -r mnt; do
    [[ -n "$mnt" ]] || continue

    if [[ "$mnt" == *:* ]]; then
      host=$(echo "$mnt" | cut -d: -f1)
      container=$(echo "$mnt" | cut -d: -f2-)
    else
      host="$mnt"
      container="$mnt"
    fi

    case "$TOOL" in
      docker|podman)
        MOUNT_ARGS+=(--mount "type=bind,source=${host},target=${container}")
        ;;
      singularity|apptainer)
        MOUNT_ARGS+=(--bind "${host}:${container}")
        ;;
    esac
  done < <($script_dir/ct_mount_detector.sh $MOUNT_DETECTOR_ARGS)
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
    echo "${CMD[@]}"
  else
    exec "${CMD[@]}"
  fi;
}

launcher_preamble() {
  determine_container_tool $1 || exit 1

  shift;

  # Rebuild the argument list without the tool.
  ARGS=("$@")

  # User details and working directory.
  USER_ID="$(id -u)"
  GROUP_ID="$(id -g)"
  PWD_DIR="$PWD"

  build_mount_args

  build_env_args
}
