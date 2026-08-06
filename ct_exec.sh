#!/usr/bin/env bash

# Improve launchability of container systems

set -euo pipefail

script_dir=$(dirname "$(realpath "$0")")

. "$script_dir/ct_library.sh"

launcher_preamble "$@"
build_payload_args exec
ct_host_projection_prepare_foreground

# Launch container with proper arguments.
case "${TOOL[0]}" in
  docker)
    CMD=("${TOOL[@]}" run --rm "${CT_HOST_PROJECTION_RUNTIME_LABEL_ARGS[@]}" --user "$USER_ID:$GROUP_ID" "${CT_HOST_PROJECTION_GROUP_ARGS[@]}" -w "$PWD_DIR"
      "${ENV_ARGS[@]}" "${MOUNT_ARGS[@]}" "${PAYLOAD_ARGS[@]}")
    ;;
  podman)
    CMD=("${TOOL[@]}" run --rm "${CT_HOST_PROJECTION_RUNTIME_LABEL_ARGS[@]}" --userns=keep-id --user "$USER_ID:$GROUP_ID" "${CT_HOST_PROJECTION_GROUP_ARGS[@]}" -w "$PWD_DIR"
      "${ENV_ARGS[@]}" "${MOUNT_ARGS[@]}" "${PAYLOAD_ARGS[@]}")
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
