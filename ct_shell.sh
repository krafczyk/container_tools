#!/usr/bin/env bash

# Improve launchability of container systems

set -euo pipefail

script_dir=$(dirname "$(realpath "$0")")

. "$script_dir/ct_library.sh"

launcher_preamble "$@"
build_payload_args shell
ct_host_projection_prepare_foreground

# Launch container with proper arguments.
case "${TOOL[0]}" in
  docker)
    if [[ -n $CT_BOOTSTRAP ]]; then
      CMD=("${TOOL[@]}" run --rm "${CT_HOST_PROJECTION_RUNTIME_LABEL_ARGS[@]}" -it --user "$USER_ID:$GROUP_ID" "${CT_HOST_PROJECTION_GROUP_ARGS[@]}" -w "$PWD_DIR"
        "${ENV_ARGS[@]}" "${MOUNT_ARGS[@]}" "${PAYLOAD_ARGS[@]}")
    else
      CMD=("${TOOL[@]}" run --rm "${CT_HOST_PROJECTION_RUNTIME_LABEL_ARGS[@]}" -it --user "$USER_ID:$GROUP_ID" "${CT_HOST_PROJECTION_GROUP_ARGS[@]}" -w "$PWD_DIR"
        "${ENV_ARGS[@]}" "${MOUNT_ARGS[@]}" "${PAYLOAD_ARGS[@]}" "$SHELL" -i)
    fi
    ;;
  podman)
    if [[ -n $CT_BOOTSTRAP ]]; then
      CMD=("${TOOL[@]}" run --rm "${CT_HOST_PROJECTION_RUNTIME_LABEL_ARGS[@]}" -it --userns=keep-id --user "$USER_ID:$GROUP_ID" "${CT_HOST_PROJECTION_GROUP_ARGS[@]}" -w "$PWD_DIR"
        "${ENV_ARGS[@]}" "${MOUNT_ARGS[@]}" "${PAYLOAD_ARGS[@]}")
    else
      CMD=("${TOOL[@]}" run --rm "${CT_HOST_PROJECTION_RUNTIME_LABEL_ARGS[@]}" -it --userns=keep-id --user "$USER_ID:$GROUP_ID" "${CT_HOST_PROJECTION_GROUP_ARGS[@]}" -w "$PWD_DIR"
        "${ENV_ARGS[@]}" "${MOUNT_ARGS[@]}" "${PAYLOAD_ARGS[@]}" "$SHELL" -i)
    fi
    ;;
  singularity|apptainer)
    if [[ -n $CT_BOOTSTRAP ]]; then
      CMD=("${TOOL[@]}" exec --pwd "$PWD_DIR" "${MOUNT_ARGS[@]}"
        --env "SINGULARITYENV_USER=$(whoami)" "${ENV_ARGS[@]}" "${PAYLOAD_ARGS[@]}")
    else
      CMD=("${TOOL[@]}" shell --pwd "$PWD_DIR" "${MOUNT_ARGS[@]}"
        --env "SINGULARITYENV_USER=$(whoami)" "${ENV_ARGS[@]}" "${PAYLOAD_ARGS[@]}")
    fi

    ;;
  *)
    echo "Unsupported tool: $TOOL"
    exit 1
    ;;
esac

run_cmd
