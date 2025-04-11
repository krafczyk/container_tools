#!/usr/bin/env bash

# Improve launchability of container systems

set -euo pipefail

script_dir=$(dirname $(realpath $0))

. $script_dir/ct_library.sh

launcher_preamble "$@"

# Launch container with proper arguments.
case "${TOOL[0]}" in
  docker)
    CMD=(
      "${TOOL[@]}"
      --user "$USER_ID:$GROUP_ID"
      -w "$PWD_DIR"
      "${ENV_ARGS[@]}"
      "${MOUNT_ARGS[@]}"
      "${ARGS[@]}"
      $SHELL
      -i
    )
    ;;
  podman)
    CMD=(
      "${TOOL[@]}"
      --userns=keep-id
      --user "$USER_ID:$GROUP_ID"
      -w "$PWD_DIR"
      "${ENV_ARGS[@]}"
      "${MOUNT_ARGS[@]}"
      "${ARGS[@]}"
      $SHELL
      -i
    )
    ;;
  singularity|apptainer)
    BASE_ARGS+=(--pwd "$PWD_DIR")
    CMD=(
      "${TOOL[@]}"
      shell
      "${MOUNT_ARGS[@]}"
      --env "SINGULARITYENV_USER=$(whoami)"
      "${SUB_ARGS[@]}"
      "${ARGS[@]}"
    )

    ;;
  *)
    echo "Unsupported tool: $TOOL"
    exit 1
    ;;
esac

run_cmd
