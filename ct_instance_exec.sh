#!/usr/bin/env bash

# Execute a command in a persistent Apptainer/SingularityCE instance. The
# instance owns image mounts needed by services that outlive this invocation.
set -euo pipefail

script_dir=$(dirname "$(realpath "$0")")
lock_timeout=${CT_INSTANCE_LOCK_TIMEOUT:-10}
probe_timeout=${CT_INSTANCE_PROBE_TIMEOUT:-5}
start_timeout=${CT_INSTANCE_START_TIMEOUT:-30}

. "$script_dir/ct_library.sh"

if [[ $# -lt 3 ]]; then
  echo "Error: container backend and --ct-instance-root are required" >&2
  exit 1
fi

backend=$1
shift
if [[ ${1:-} != --ct-instance-root || $# -lt 2 ]]; then
  echo "Error: --ct-instance-root must follow the container backend" >&2
  exit 1
fi
instance_root=$2
shift 2

if [[ $instance_root != /* || $instance_root == *$'\n'* ]]; then
  echo "Error: --ct-instance-root requires an absolute path without newlines" >&2
  exit 1
fi

launcher_preamble "$backend" "$@"
case "${TOOL[0]}" in
  apptainer|singularity) ;;
  *)
    echo "Error: persistent instances require Apptainer or SingularityCE" >&2
    exit 1
    ;;
esac
if [[ -z $CT_BOOTSTRAP && ${ARGS[0]:-} == -- ]]; then
  ARGS=("${ARGS[@]:1}")
fi
build_payload_args exec

unset SINGULARITY_BIND SINGULARITY_BINDPATH SINGULARITY_MOUNT
unset APPTAINER_BIND APPTAINER_BINDPATH APPTAINER_MOUNT

if [[ ${#ARGS[@]} -lt 2 || ${ARGS[0]} != /* || ! -f ${ARGS[0]} ]]; then
  echo "Error: persistent instance execution requires an absolute container image and command" >&2
  exit 1
fi

instance_root=$(realpath -m -- "$instance_root")
if [[ -e $instance_root ]]; then
  if [[ ! -d $instance_root || -L $instance_root \
    || $(stat -Lc '%u' -- "$instance_root") != "$USER_ID" \
    || $(stat -Lc '%a' -- "$instance_root") != 700 ]]; then
    echo "Error: instance root must be a current-user-owned mode-0700 directory" >&2
    exit 1
  fi
else
  install -d -m 700 -- "$instance_root"
fi

image=${ARGS[0]}
image_real=$(realpath -- "$image")
image_identity=$(stat -Lc '%d:%i:%s:%y:%z' -- "$image_real")
IFS= read -r host_name < /proc/sys/kernel/hostname
user_name=$(whoami)
bootstrap_identity=
if [[ -n $CT_BOOTSTRAP ]]; then
  bootstrap_real=$(realpath -- "$CT_BOOTSTRAP")
  bootstrap_identity="$bootstrap_real:$(stat -Lc '%d:%i:%s:%y:%z' -- "$bootstrap_real")"
  for ((index = 0; index < ${#MOUNT_ARGS[@]}; index++)); do
    if [[ ${MOUNT_ARGS[index]} == --bind \
      && ${MOUNT_ARGS[index + 1]:-} == "$CT_BOOTSTRAP:$CT_BOOTSTRAP_CONTAINER:ro" ]]; then
      MOUNT_ARGS[index + 1]="$bootstrap_real:$CT_BOOTSTRAP_CONTAINER:ro"
    fi
  done
fi

assets_unchanged() {
  local current_real
  local current_identity
  current_real=$(realpath -- "$image" 9>&-) || return 1
  current_identity=$(stat -Lc '%d:%i:%s:%y:%z' -- "$image_real" 9>&-) || return 1
  [[ $current_real == "$image_real" && $current_identity == "$image_identity" ]] || return 1
  if [[ -n $CT_BOOTSTRAP ]]; then
    current_real=$(realpath -- "$CT_BOOTSTRAP" 9>&-) || return 1
    current_identity=$(stat -Lc '%d:%i:%s:%y:%z' -- "$bootstrap_real" 9>&-) || return 1
    [[ $current_real == "$bootstrap_real" \
      && "$bootstrap_real:$current_identity" == "$bootstrap_identity" ]] || return 1
  fi
}

# Instances keep their initial bind namespace. Preserve the foreground launcher's
# implicit working-directory bind only when HOME or an existing same-path bind
# does not already cover it.
pwd_real=$(realpath -- "$PWD_DIR")
home_real=$(realpath -m -- "$HOME")
pwd_covered=0
case "$pwd_real/" in
  "$home_real/"*) pwd_covered=1 ;;
esac
bind_identities=()
for ((index = 0; index < ${#MOUNT_ARGS[@]}; index++)); do
  [[ ${MOUNT_ARGS[index]} == --bind && $((index + 1)) -lt ${#MOUNT_ARGS[@]} ]] || continue
  mount=${MOUNT_ARGS[index + 1]}
  source_path=${mount%%:*}
  destination=${mount#*:}
  destination=${destination%%:*}
  source_real=$(realpath -- "$source_path")
  source_identity=$(stat -Lc '%d:%i:%F' -- "$source_real")
  bind_identities+=("$mount:$source_real:$source_identity")
  case "$pwd_real/" in
    "$source_real/"*)
      mapped="$destination${pwd_real#"$source_real"}"
      [[ $mapped != "$pwd_real" ]] || pwd_covered=1
      ;;
  esac
done
if [[ $pwd_covered -eq 0 ]]; then
  append_mount_arg "$pwd_real" "$pwd_real"
  bind_identities+=("$pwd_real:$pwd_real:$pwd_real:$(stat -Lc '%d:%i:%F' -- "$pwd_real")")
fi

profile=$(
  {
    printf '%s\0' "${TOOL[@]}" "$USER_ID" "$host_name" "$HOME" \
      "$instance_root" "$image_real" "$image_identity" "$bootstrap_identity"
    printf '%s\0' "${MOUNT_ARGS[@]}" "${bind_identities[@]}"
  } | sha256sum
)
profile=${profile%% *}
instance_name="mkchad-${profile:0:32}"
instance_uri="instance://$instance_name"

exec 9>"$instance_root/$instance_name.lock"
if ! flock -w "$lock_timeout" 9; then
  echo "Error: timed out waiting for persistent instance creation" >&2
  exit 1
fi
if ! assets_unchanged; then
  echo "Error: container image or bootstrap changed while preparing its instance" >&2
  exit 1
fi

probe=("${TOOL[@]}" exec "$instance_uri" /bin/true)
probe_instance() {
  timeout --foreground --kill-after=1s "${probe_timeout}s" "${probe[@]}" 9>&- >/dev/null 2>&1
}

if ! probe_instance; then
  start=(
    "${TOOL[@]}" instance start
    "${MOUNT_ARGS[@]}"
    --env "SINGULARITYENV_USER=$user_name"
    "${ENV_ARGS[@]}"
    "$image_real"
    "$instance_name"
  )
  start_succeeded=0
  timeout --foreground --kill-after=1s "${start_timeout}s" "${start[@]}" 9>&- 1>&2 && start_succeeded=1
  if [[ $start_succeeded -eq 0 ]] && ! probe_instance; then
    echo "Error: persistent container instance failed to start" >&2
    exit 1
  fi
  if ! probe_instance; then
    echo "Error: persistent container instance did not become executable" >&2
    exit 1
  fi
fi
if ! assets_unchanged; then
  echo "Error: container image or bootstrap changed while preparing its instance" >&2
  exit 1
fi

metadata="$instance_root/$instance_name.identity"
metadata_tmp="$metadata.tmp.$$"
if ! (
  exec 9>&-
  umask 077
  printf 'name=%s\nimage=%s\nidentity=%s\n' "$instance_name" "$image_real" "$image_identity" > "$metadata_tmp" \
    && mv -f -- "$metadata_tmp" "$metadata"
); then
  rm -f -- "$metadata_tmp" 9>&-
  echo "WARNING: unable to update persistent instance metadata: $metadata" >&2
fi

flock -u 9
exec 9>&-

# shellcheck disable=SC2034 # run_cmd consumes CMD from the sourced library.
CMD=(
  "${TOOL[@]}"
  exec
  --pwd "$PWD_DIR"
  --env "SINGULARITYENV_USER=$user_name"
  "${ENV_ARGS[@]}"
  "$instance_uri"
  "${PAYLOAD_ARGS[@]:1}"
)
run_cmd
