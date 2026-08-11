#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
set -euo pipefail
export LC_ALL=C

work=${1:?pass the completed native instance fixture root}
helper=${2:?pass the Bash instance helper}
native=${3:?pass the installed native executable}
real_id=$(command -v id)

export HOME="$work/home"
export PATH="$work/fake-bin:$PATH"
export CT_MOUNT_CFG="$work/mount-config"
export MKCHAD_TEST_CALLS="$work/calls"
export MKCHAD_TEST_CALL_COUNT="$work/call-count"
export MKCHAD_TEST_CALL_LOCK="$work/call.lock"
export MKCHAD_TEST_INSTANCES="$work/instances"
export MKCHAD_TEST_HIDE_INSTANCE_LIST=1
MKCHAD_TEST_GROUPS=$($real_id -G)
export MKCHAD_TEST_GROUPS
export CT_HOST_PROJECTION_GROUPS="$MKCHAD_TEST_GROUPS"
export CT_HOST_PROJECTION_CACHE_ROOT="$work/projection-cache"
export CT_MOUNT_PLAN_STATE_ROOT="$work/mount-plans"
export CT_HOST_PROJECTION_MOUNTINFO="$work/mountinfo"
export CT_HOST_PROJECTION_SOURCE_ROOT="$work/projection root"
export CT_HOST_PROJECTION_HOSTNAME=instance-test-host
export CT_HOST_PROJECTION_EXECUTABLE="$work/fake-bin/apptainer"
export CT_HOST_PROJECTION_BOOT_ID=instance-test-boot
shopt -s nullglob
selection_records=("$work/projection-cache"/[!.]*)
selection_count=${#selection_records[@]}

starts_before=$(for call in "$work/calls"/*; do
  mapfile -t argv < "$call"
  if [[ ${argv[0]:-} == instance && ${argv[1]:-} == start ]]; then printf 'start\n'; fi
done | wc -l)

expected_profile=
call_count=$(<"$work/call-count")
for ((call_number = 1; call_number <= call_count; call_number++)); do
  call="$work/calls/$call_number"
  [[ -f $call ]] || continue
  candidate_profile=
  has_literal=0
  while IFS= read -r argument; do
    case "$argument" in
      SINGULARITYENV_CONTAINER_TOOLS_PROFILE=*)
        candidate_profile=${argument#*=}
        ;;
      'literal argument with spaces') has_literal=1 ;;
    esac
  done < "$call"
  if (( has_literal )) && [[ -n $candidate_profile ]]; then expected_profile=$candidate_profile; fi
done
[[ $expected_profile =~ ^[0-9a-f]{64}$ ]] || {
  printf '%s\n' 'native fixture did not expose its canonical profile' >&2
  exit 1
}

calls_before=$(<"$work/call-count")
identity=$("$native" instance identity --apptainer \
  --ct-instance-root "$work/instance root" \
  --ct-bind "$work/projection root/bind source:$work/container bind" \
  --ct-env 'TEST_VALUE=space value' \
  --ct-bootstrap "$work/bootstrap" \
  -- "$work/image.sif" /bin/fake-command 'literal argument with spaces' '' '--leading-dash' '*' \
  2>"$work/native-identity.err")
json=$("$native" instance identity --apptainer \
  --ct-instance-root "$work/instance root" \
  --ct-bind "$work/projection root/bind source:$work/container bind" \
  --ct-env 'TEST_VALUE=space value' \
  --ct-bootstrap "$work/bootstrap" \
  -- "$work/image.sif" /bin/fake-command 'literal argument with spaces' '' '--leading-dash' '*' --json)
ln -s "$work" "$work/root-alias"
alias_identity=$("$native" instance identity --apptainer \
  --ct-instance-root "$work/root-alias/instance root" \
  --ct-bind "$work/projection root/bind source:$work/container bind" \
  --ct-env 'TEST_VALUE=space value' \
  --ct-bootstrap "$work/bootstrap" \
  -- "$work/image.sif" /bin/fake-command 'literal argument with spaces' '' '--leading-dash' '*')
[[ $identity == "$expected_profile" \
  && $alias_identity == "$identity" \
  && $json == "{\"schema\":\"container-tools.instance-identity/v1\",\"identity\":\"$identity\"}" \
  && ! -s "$work/native-identity.err" \
  && $(<"$work/call-count") == "$calls_before" ]] || {
  printf '%s\n' 'instance identity contacted a runtime or diverged from instance exec' >&2
  exit 1
}
set +e
failed_identity=$("$native" instance identity --apptainer \
  --ct-instance-root "$work/identity-failure-root" -- \
  "$work/missing-image.sif" /bin/true 2>/dev/null)
failed_status=$?
set -e
[[ $failed_status -ne 0 && -z $failed_identity \
  && ! -e "$work/identity-failure-root" \
  && $(<"$work/call-count") == "$calls_before" ]] || {
  printf '%s\n' 'failed identity preparation emitted a digest or contacted an instance' >&2
  exit 1
}

set +e
"$helper" --apptainer --ct-instance-root "$work/instance root" \
  --ct-bind "$work/projection root/bind source:$work/container bind" \
  --ct-env 'TEST_VALUE=space value' \
  --ct-bootstrap "$work/bootstrap" \
  -- "$work/image.sif" /bin/fake-command 'literal argument with spaces' '' '--leading-dash' '*'
status=$?
set -e

starts_after=$(for call in "$work/calls"/*; do
  mapfile -t argv < "$call"
  if [[ ${argv[0]:-} == instance && ${argv[1]:-} == start ]]; then printf 'start\n'; fi
done | wc -l)
selection_records=("$work/projection-cache"/[!.]*)
[[ $status -eq 23 && $starts_after -eq $starts_before \
  && ${#selection_records[@]} -eq "$selection_count" ]] || {
  printf '%s\n' 'Bash caller did not reuse the native canonical profile' >&2
  exit 1
}

count_starts() {
  local call count=0
  for call in "$work/calls"/*; do
    mapfile -t argv < "$call"
    if [[ ${argv[0]:-} == instance && ${argv[1]:-} == start ]]; then ((++count)); fi
  done
  printf '%s\n' "$count"
}

race_root="$work/mixed race root"
race_args=(--apptainer --ct-instance-root "$race_root" --ct-env MIXED_RACE=1 -- \
  "$work/image.sif" /bin/fake-command mixed-race)
starts_before=$(count_starts)
set +e
"$native" instance exec "${race_args[@]}" >"$work/native-race.out" 2>"$work/native-race.err" &
native_pid=$!
"$helper" "${race_args[@]}" >"$work/bash-race.out" 2>"$work/bash-race.err" &
bash_pid=$!
wait "$native_pid"
native_status=$?
wait "$bash_pid"
bash_status=$?
set -e
starts_after=$(count_starts)
race_pending=("$race_root"/*.pending)
[[ $native_status -eq 23 && $bash_status -eq 23 \
  && $starts_after -eq $((starts_before + 1)) && ${#race_pending[@]} -eq 0 ]] || {
  printf '%s\n' 'mixed first use did not serialize one canonical instance start' >&2
  exit 1
}

adopt_pending() {
  local pending=$1
  local -a fields
  mapfile -t fields < "$pending"
  [[ ${#fields[@]} -eq 3 ]] || return 1
  : > "$work/instances/${fields[0]}"
  printf '%s\n' "${fields[1]}" > "$work/instances/${fields[0]}.profile"
  printf '%s\n' "${fields[2]}" > "$work/instances/${fields[0]}.nonce"
}

bash_pending_root="$work/bash pending root"
bash_pending_args=(--apptainer --ct-instance-root "$bash_pending_root" \
  --ct-env MIXED_PENDING=BASH -- "$work/image.sif" /bin/fake-command bash-pending)
set +e
CT_INSTANCE_CREATION_NONCE=11111111111111111111111111111111 \
  MKCHAD_TEST_HANG_START=1 CT_INSTANCE_START_TIMEOUT=0.1 \
  "$helper" "${bash_pending_args[@]}" >"$work/bash-pending.out" 2>"$work/bash-pending.err"
bash_pending_status=$?
set -e
bash_pending_files=("$bash_pending_root"/*.pending)
[[ $bash_pending_status -ne 0 && ${#bash_pending_files[@]} -eq 1 ]] || {
  printf '%s\n' 'Bash writer did not retain a canonical pending record' >&2
  exit 1
}
adopt_pending "${bash_pending_files[0]}"
set +e
"$native" instance exec "${bash_pending_args[@]}" >"$work/native-adopt.out" 2>"$work/native-adopt.err"
native_adopt_status=$?
set -e
[[ $native_adopt_status -eq 23 && ! -e ${bash_pending_files[0]} ]] || {
  printf '%s\n' 'native caller did not adopt the Bash pending record' >&2
  exit 1
}

native_pending_root="$work/native pending root"
native_pending_args=(--apptainer --ct-instance-root "$native_pending_root" \
  --ct-env MIXED_PENDING=NATIVE -- "$work/image.sif" /bin/fake-command native-pending)
set +e
CT_INSTANCE_CREATION_NONCE=22222222222222222222222222222222 \
  MKCHAD_TEST_HANG_START=1 CT_INSTANCE_START_TIMEOUT=0.1 \
  "$native" instance exec "${native_pending_args[@]}" >"$work/native-pending.out" 2>"$work/native-pending.err"
native_pending_status=$?
set -e
native_pending_files=("$native_pending_root"/*.pending)
[[ $native_pending_status -ne 0 && ${#native_pending_files[@]} -eq 1 ]] || {
  printf '%s\n' 'native writer did not retain a canonical pending record' >&2
  exit 1
}
adopt_pending "${native_pending_files[0]}"
set +e
"$helper" "${native_pending_args[@]}" >"$work/bash-adopt.out" 2>"$work/bash-adopt.err"
bash_adopt_status=$?
set -e
[[ $bash_adopt_status -eq 23 && ! -e ${native_pending_files[0]} ]] || {
  printf '%s\n' 'Bash caller did not adopt the native pending record' >&2
  exit 1
}
