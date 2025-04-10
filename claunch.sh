#!/usr/bin/env bash
set -euo pipefail

# Determine container tool from required flag.
TOOL=""
for arg in "$@"; do
  case "$arg" in
    --docker)      TOOL="docker"; shift; break ;;
    --podman)      TOOL="podman"; shift; break ;;
    --singularity) TOOL="singularity"; shift; break ;;
    --apptainer)   TOOL="apptainer"; shift; break ;;
  esac
done

if [[ -z "$TOOL" ]]; then
  echo "Error: Must specify one of --docker, --podman, --singularity, or --apptainer"
  exit 1
fi

# Rebuild the argument list without the tool flag.
ARGS=("$@")

# For Singularity/Apptainer, separate base from subcommand arguments.
BASE_ARGS=()
SUB_ARGS=()
if [[ "$TOOL" == "docker" || "$TOOL" == "podman" ]]; then
  BASE_ARGS=("${ARGS[@]}")
else
  base_done=false
  for arg in "${ARGS[@]}"; do
    if [[ "$base_done" == false && "$arg" != -* ]]; then
      BASE_ARGS+=("$arg")
      base_done=true
    elif [[ "$base_done" == false ]]; then
      BASE_ARGS+=("$arg")
    else
      SUB_ARGS+=("$arg")
    fi
  done
fi

# User details and working directory.
USER_ID="$(id -u)"
GROUP_ID="$(id -g)"
PWD_DIR="$PWD"

# Build mount arguments using mount_detector.sh.
MOUNT_ARGS=()
while IFS= read -r mnt; do
  [[ -n "$mnt" ]] || continue
  case "$TOOL" in
    docker|podman)
      MOUNT_ARGS+=(--mount "type=bind,source=${mnt},target=${mnt}")
      ;;
    singularity|apptainer)
      MOUNT_ARGS+=(--bind "${mnt}:${mnt}")
      ;;
  esac
done < <(./mount_detector.sh)

# Environment: pass the entire host environment.
if [[ "$TOOL" == "docker" || "$TOOL" == "podman" ]]; then
  # Process substitution returns a filename that lists all env variables.
  ENV_ARGS=(--env-file <(env))
elif [[ "$TOOL" == "singularity" || "$TOOL" == "apptainer" ]]; then
  BASE_ARGS+=("-E")
fi

# Launch container with proper arguments.
case "$TOOL" in
  docker)
    exec docker "${BASE_ARGS[@]}" \
      --user "$USER_ID:$GROUP_ID" \
      -w "$PWD_DIR" \
      "${ENV_ARGS[@]}" \
      "${MOUNT_ARGS[@]}"
    ;;
  podman)
    exec podman "${BASE_ARGS[@]}" \
      --userns=keep-id \
      --user "$USER_ID:$GROUP_ID" \
      -w "$PWD_DIR" \
      "${ENV_ARGS[@]}" \
      "${MOUNT_ARGS[@]}"
    ;;
  singularity|apptainer)
    BASE_ARGS+=(--pwd "$PWD_DIR")
    exec "$TOOL" "${BASE_ARGS[@]}" \
      "${MOUNT_ARGS[@]}" \
      "${SUB_ARGS[@]}"
    ;;
  *)
    echo "Unsupported tool: $TOOL"
    exit 1
    ;;
esac
