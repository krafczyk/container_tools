#!/usr/bin/env bash

# Improve launchability of container systems

set -euo pipefail

script_dir=$(dirname "$(realpath "$0")")

. "$script_dir/ct_library.sh"

launcher_preamble "$@"
build_payload_args exec

# Launch container with proper arguments.
case "${TOOL[0]}" in
  docker)
    if [[ -n $CT_BOOTSTRAP ]]; then
      CMD=("${TOOL[@]}" run --rm --user "$USER_ID:$GROUP_ID" -w "$PWD_DIR"
        "${ENV_ARGS[@]}" "${MOUNT_ARGS[@]}" "${PAYLOAD_ARGS[@]}")
    else
      CMD=("${TOOL[@]}" --user "$USER_ID:$GROUP_ID" -w "$PWD_DIR"
        "${ENV_ARGS[@]}" "${MOUNT_ARGS[@]}" "${PAYLOAD_ARGS[@]}")
    fi
    ;;
  podman)
    if [[ -n $CT_BOOTSTRAP ]]; then
      CMD=("${TOOL[@]}" run --rm --userns=keep-id --user "$USER_ID:$GROUP_ID" -w "$PWD_DIR"
        "${ENV_ARGS[@]}" "${MOUNT_ARGS[@]}" "${PAYLOAD_ARGS[@]}")
    else
      CMD=("${TOOL[@]}" --userns=keep-id --user "$USER_ID:$GROUP_ID" -w "$PWD_DIR"
        "${ENV_ARGS[@]}" "${MOUNT_ARGS[@]}" "${PAYLOAD_ARGS[@]}")
    fi
    ;;
  singularity|apptainer)
    CMD=(
      "${TOOL[@]}"
      exec
      --pwd "$PWD_DIR"
      "${MOUNT_ARGS[@]}"
      --env "SINGULARITYENV_USER=$(whoami)"
      "${ENV_ARGS[@]}"
      "${PAYLOAD_ARGS[@]}"
    )

    ;;
  *)
    echo "Unsupported tool: $TOOL"
    exit 1
    ;;
esac

run_cmd
