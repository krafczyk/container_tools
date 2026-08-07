#!/usr/bin/env bash

# Execute a command in a persistent Apptainer/SingularityCE instance. The
# instance owns image mounts needed by services that outlive this invocation.
set -euo pipefail

script_dir=$(dirname "$(realpath "$0")")
lock_timeout=${CT_INSTANCE_LOCK_TIMEOUT:-10}
probe_timeout=${CT_INSTANCE_PROBE_TIMEOUT:-5}
start_timeout=${CT_INSTANCE_START_TIMEOUT:-30}
instance_profile_version=ct-instance-profile-v2
identity_path=/.container-tools-instance-identity

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

if [[ $instance_root != /* || $instance_root == *:* || $instance_root == *,* || $instance_root == *$'\n'* ]]; then
  echo "Error: --ct-instance-root requires an absolute path without colon, comma, or newline" >&2
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

if [[ -n ${CT_DRY_RUN:-} ]]; then
  # Persistent dry runs must not create an instance root, selection record, or
  # pending journal. The printed command remains useful without claiming it is
  # attached to an existing service.
  CMD=("${CT_RUNTIME_TOOL[@]}" exec --pwd "$PWD_DIR" --env "SINGULARITYENV_USER=$(whoami)" \
    "${ENV_ARGS[@]}" instance://dry-run "${PAYLOAD_ARGS[@]:1}")
  run_cmd
  exit 0
fi

# This consumes a memoized selection on the warm path and prepends all
# generated binds before the instance profile is finalized or started.
ct_host_projection_prepare_foreground

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
generated_mount_arg_count=${#CT_HOST_PROJECTION_MOUNT_ARGS[@]}
bind_identities=()
for ((index = 0; index < ${#MOUNT_ARGS[@]}; index++)); do
  # Generated binds have a separate semantic profile. Retaining their stat
  # identity here would churn instances for harmless remount metadata changes.
  (( index < generated_mount_arg_count )) && continue
  [[ ${MOUNT_ARGS[index]} == --bind && $((index + 1)) -lt ${#MOUNT_ARGS[@]} ]] || continue
  mount=${MOUNT_ARGS[index + 1]}
  source_path=${mount%%:*}
  destination=${mount#*:}
  destination=${destination%%:*}
  if [[ $destination == "$identity_path" ]]; then
    echo "Error: container bind destination is reserved for persistent instance identity: $identity_path" >&2
    exit 1
  fi
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

ct_host_projection_profile_digest "${TOOL[0]}"
mapfile -t supplementary_groups < <(id -G | tr ' ' '\n' | LC_ALL=C sort -n -u)
effective_supplementary_groups=()
for group in "${supplementary_groups[@]}"; do
  [[ $group == "$GROUP_ID" ]] || effective_supplementary_groups+=("$group")
done
profile=$(
  {
    printf '%s\0' "${TOOL[@]}" "$USER_ID" "$host_name" "$HOME" \
      "$instance_root" "$image_real" "$image_identity" "$bootstrap_identity"
    printf '%s\0' "$instance_profile_version"
    printf '%s\0' "$CT_HOST_PROJECTION_PROFILE_VERSION" "$CT_HOST_PROJECTION_PROFILE_DIGEST"
    printf '%s\0' "$USER_ID" "$GROUP_ID" "${CT_HOST_PROJECTION_GROUP_MODE:-none}"
    printf '%s\0' "${effective_supplementary_groups[@]}"
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

# A matching name is not sufficient: a pre-existing instance must have been
# started with this complete semantic profile before it can receive a payload.
probe_instance() {
  local expected_profile=$1 expected_nonce=${2:-}
  # shellcheck disable=SC2016 # The instance shell reads its pinned identity record.
  local -a probe=("${CT_RUNTIME_TOOL[@]}" exec "$instance_uri" /bin/sh -c '
    exec 3</.container-tools-instance-identity || exit 42
    IFS= read -r actual_name <&3 || exit 42
    IFS= read -r actual_profile <&3 || exit 42
    IFS= read -r actual_nonce <&3 || exit 42
    [ "$actual_name" = "$1" ] || exit 42
    [ "$actual_profile" = "$2" ] || exit 42
    [ -z "${3:-}" ] || [ "$actual_nonce" = "$3" ] || exit 42
  ' sh "$instance_name" "$expected_profile" "$expected_nonce")
  timeout --foreground --kill-after=1s "${probe_timeout}s" "${probe[@]}" 9>&- >/dev/null 2>&1
}

# Return success only for the backend's structured empty instance list. Any
# timeout, runtime error, malformed result, or listed instance is ambiguous and
# cannot authorize replacement of a pending creation nonce.
instance_is_conclusively_absent() {
  local instances
  if instances=$(timeout --foreground --kill-after=1s "${probe_timeout}s" \
    "${CT_RUNTIME_TOOL[@]}" instance list --json "$instance_name" 9>&- 2>/dev/null); then
    [[ $instances =~ ^[[:space:]]*\[[[:space:]]*\][[:space:]]*$ \
      || $instances =~ ^[[:space:]]*\{[[:space:]]*\"instances\"[[:space:]]*:[[:space:]]*\[[[:space:]]*\][[:space:]]*\}[[:space:]]*$ ]]
  else
    return 1
  fi
}

pending="$instance_root/$instance_name.pending"
instance_ready=0
pending_restart=0
if [[ -e $pending ]]; then
  pending_mode=$(stat -Lc '%a' -- "$pending" 2>/dev/null || true)
  mapfile -t pending_fields < "$pending" || true
  if [[ $pending_mode != 600 || ${#pending_fields[@]} != 3 \
    || ${pending_fields[0]} != "$instance_name" || ${pending_fields[1]} != "$profile" \
    || ! ${pending_fields[2]} =~ ^[0-9a-f]{32}$ ]]; then
    echo "Error: persistent instance pending record does not match the requested profile" >&2
    exit 1
  fi
  pending_nonce=${pending_fields[2]}
  if probe_instance "$profile" "$pending_nonce"; then
    rm -f -- "$pending"
    instance_ready=1
  else
    pending_status=$?
    if (( pending_status == 42 )); then
      if probe_instance "$profile"; then
        echo "Error: persistent instance pending nonce mismatch" >&2
      else
        echo "Error: persistent instance profile mismatch" >&2
      fi
      exit 1
    fi
    if ! instance_is_conclusively_absent; then
      echo "Error: unable to reconcile pending persistent instance creation" >&2
      exit 1
    fi
    # A delayed first start may appear after the list result. Retain its nonce
    # and retry the same named creation so any later adoption remains exact.
    pending_restart=1
  fi
fi

if [[ $instance_ready -eq 0 && $pending_restart -eq 0 ]]; then
  if probe_instance "$profile"; then
    instance_ready=1
  else
    probe_status=$?
    if (( probe_status == 42 )); then
      echo "Error: persistent instance profile mismatch" >&2
      exit 1
    fi
    if (( probe_status == 124 || probe_status == 137 )); then
      echo "Error: persistent container instance liveness check timed out" >&2
      exit 1
    fi
    if ! instance_is_conclusively_absent; then
      echo "Error: unable to reconcile persistent instance liveness failure" >&2
      exit 1
    fi
  fi
fi

if [[ $instance_ready -eq 0 ]]; then
  nonce=${pending_nonce:-${CT_INSTANCE_CREATION_NONCE:-}}
  [[ -n $nonce ]] || nonce=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
  [[ $nonce =~ ^[0-9a-f]{32}$ ]] || {
    echo "Error: CT_INSTANCE_CREATION_NONCE must be 32 lowercase hexadecimal characters" >&2
    exit 1
  }
  pending_tmp=$(mktemp "$instance_root/.${instance_name}.pending.XXXXXX")
  (
    umask 077
    printf '%s\n%s\n%s\n' "$instance_name" "$profile" "$nonce" > "$pending_tmp"
    chmod 600 -- "$pending_tmp"
    mv -f -- "$pending_tmp" "$pending"
  ) || {
    rm -f -- "$pending_tmp"
    echo "Error: unable to record pending persistent instance creation" >&2
    exit 1
  }
  start=(
    "${CT_RUNTIME_TOOL[@]}" instance start
    "${MOUNT_ARGS[@]}"
    --bind "$pending:$identity_path:ro"
    --env "SINGULARITYENV_USER=$user_name"
    "${ENV_ARGS[@]}"
    "$image_real"
    "$instance_name"
  )
  start_succeeded=0
  timeout --foreground --kill-after=1s "${start_timeout}s" "${start[@]}" 9>&- 1>&2 && start_succeeded=1
  if probe_instance "$profile" "$nonce"; then
    :
  else
    probe_status=$?
    if (( probe_status == 42 )); then
      echo "Error: persistent instance profile mismatch" >&2
    elif [[ $start_succeeded -eq 0 ]]; then
      echo "Error: persistent container instance failed to start" >&2
    else
      echo "Error: persistent container instance did not become executable" >&2
    fi
    exit 1
  fi
  if ! rm -f -- "$pending"; then
    echo "Error: unable to clear pending persistent instance creation" >&2
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
  printf 'name=%s\nimage=%s\nidentity=%s\nprofile=%s\n' "$instance_name" "$image_real" "$image_identity" "$profile" > "$metadata_tmp" \
    && mv -f -- "$metadata_tmp" "$metadata"
); then
  rm -f -- "$metadata_tmp" 9>&-
  echo "WARNING: unable to update persistent instance metadata: $metadata" >&2
fi

flock -u 9
exec 9>&-

# shellcheck disable=SC2034 # run_cmd consumes CMD from the sourced library.
CMD=(
  "${CT_RUNTIME_TOOL[@]}"
  exec
  --pwd "$PWD_DIR"
  --env "SINGULARITYENV_USER=$user_name"
  "${ENV_ARGS[@]}"
  "$instance_uri"
  "${PAYLOAD_ARGS[@]:1}"
)
run_cmd
