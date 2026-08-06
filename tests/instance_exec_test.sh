#!/usr/bin/env bash
set -euo pipefail

work=${1:?pass a task-specific directory beneath /tmp/mkchad-v1/host-root-projection}
helper=${2:?pass the ct_instance_exec.sh path}
helper=$(realpath "$helper")
work=$(realpath -m -- "$work")
[[ $work == /tmp/mkchad-v1/host-root-projection/* ]] || { printf '%s\n' 'test directory must be beneath /tmp/mkchad-v1/host-root-projection' >&2; exit 2; }
[[ ! -e $work ]] || { printf '%s\n' 'test directory already exists' >&2; exit 2; }

fake="$work/fake-bin"
home="$work/home"
calls="$work/calls"
instances="$work/instances"
instance_root="$work/instance root"
image_path="$work/image.sif"
bootstrap="$work/bootstrap"
projection_root="$work/projection root"
bind_source="$projection_root/bind source"
mkdir -p "$fake" "$home" "$calls" "$instances" "$bind_source"
: > "$image_path"
cat > "$bootstrap" <<'EOF'
#!/usr/bin/env bash
exec "$@"
EOF
chmod 755 "$bootstrap"

cat > "$fake/apptainer" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
[[ -z ${SINGULARITY_BIND:-}${SINGULARITY_BINDPATH:-}${SINGULARITY_MOUNT:-} ]] || exit 65
[[ -z ${APPTAINER_BIND:-}${APPTAINER_BINDPATH:-}${APPTAINER_MOUNT:-} ]] || exit 65
[[ ! -e /proc/$$/fd/9 ]] || exit 66
if [[ ${1:-} == exec && " $* " != *' instance://'* ]]; then
  printf '%s\n' 'ct-host-projection-group=native'
  exit 0
fi
exec 8>"$MKCHAD_TEST_CALL_LOCK"
flock 8
count=0
[[ ! -f $MKCHAD_TEST_CALL_COUNT ]] || read -r count < "$MKCHAD_TEST_CALL_COUNT"
count=$((count + 1))
printf '%s\n' "$count" > "$MKCHAD_TEST_CALL_COUNT"
printf '%s\n' "$@" > "$MKCHAD_TEST_CALLS/$count"
flock -u 8

if [[ ${1:-} == instance && ${2:-} == start ]]; then
  for argument in "$@"; do
    [[ $argument != --pwd ]] || exit 67
  done
  name=${!#}
  profile=
  nonce=
  for argument in "$@"; do
    [[ $argument != CT_HOST_PROJECTION_PROFILE=* ]] || profile=${argument#*=}
    [[ $argument != CT_INSTANCE_CREATION_NONCE=* ]] || nonce=${argument#*=}
  done
  [[ $profile =~ ^[0-9a-f]{64}$ && $nonce =~ ^[0-9a-f]{32}$ ]] || exit 68
  [[ ${MKCHAD_TEST_HANG_START:-} != 1 ]] || sleep 10
  sleep "${MKCHAD_TEST_START_DELAY:-0}"
  [[ ! -e $MKCHAD_TEST_INSTANCES/$name ]] || exit 42
  : > "$MKCHAD_TEST_INSTANCES/$name"
  printf '%s\n' "$profile" > "$MKCHAD_TEST_INSTANCES/$name.profile"
  printf '%s\n' "$nonce" > "$MKCHAD_TEST_INSTANCES/$name.nonce"
  exit 0
fi

if [[ ${1:-} == exec ]]; then
  uri=
  uri_index=0
  index=1
  for argument in "$@"; do
    if [[ $argument == instance://* ]]; then
      uri=$argument
      uri_index=$index
      break
    fi
    index=$((index + 1))
  done
  [[ -n $uri && -e $MKCHAD_TEST_INSTANCES/${uri#instance://} ]] || exit 43
  eval "command=\${$((uri_index + 1)):-}"
  if [[ $command == /bin/sh ]]; then
    [[ ${MKCHAD_TEST_HANG_PROBE:-} != 1 ]] || sleep 10
    if [[ -n ${MKCHAD_TEST_MUTATE_ON_PROBE:-} && ! -e ${MKCHAD_TEST_MUTATE_MARKER:-} ]]; then
      printf 'mutated during probe\n' >> "$MKCHAD_TEST_MUTATE_ON_PROBE"
      : > "$MKCHAD_TEST_MUTATE_MARKER"
    fi
    eval "expected_profile=\${$((uri_index + 5)):-}"
    eval "expected_nonce=\${$((uri_index + 6)):-}"
    [[ ${MKCHAD_TEST_FORCE_PROFILE_MISMATCH:-} != 1 ]] || exit 42
    [[ $expected_profile == "$(<"$MKCHAD_TEST_INSTANCES/${uri#instance://}.profile")" ]] || exit 42
    [[ -z $expected_nonce || $expected_nonce == "$(<"$MKCHAD_TEST_INSTANCES/${uri#instance://}.nonce")" ]] || exit 42
    exit 0
  fi
  exit "${MKCHAD_TEST_PAYLOAD_STATUS:-23}"
fi

exit 64
EOF
chmod 755 "$fake/apptainer"

real_id=$(command -v id)
primary_group=$($real_id -g)
cat > "$fake/id" <<EOF
#!/usr/bin/env bash
case "\${1:-}" in
  -G) printf '%s\\n' "\${MKCHAD_TEST_GROUPS:-$($real_id -G)}" ;;
  *) exec "$real_id" "\$@" ;;
esac
EOF
chmod 755 "$fake/id"

export HOME="$home"
export PATH="$fake:$PATH"
export CT_MOUNT_CFG="$work/missing-mount-config"
export MKCHAD_TEST_CALLS="$calls"
export MKCHAD_TEST_CALL_COUNT="$work/call-count"
export MKCHAD_TEST_CALL_LOCK="$work/call.lock"
export MKCHAD_TEST_INSTANCES="$instances"
MKCHAD_TEST_GROUPS="$($real_id -G)"
export MKCHAD_TEST_GROUPS

# Seed the projection selection record so persistent warm-path assertions do
# not need a capability runtime probe. The instance helper must consume this
# record and add the generated bind before finalizing its profile.
mountinfo="$work/mountinfo"
printf '24 1 8:1 / / rw,relatime - ext4 /dev/root rw\n25 24 8:2 / %s rw,relatime - ext4 /dev/test rw\n' "$bind_source" > "$mountinfo"
export CT_HOST_PROJECTION_CACHE_ROOT="$work/projection-cache"
export CT_HOST_PROJECTION_MOUNTINFO="$mountinfo"
export CT_HOST_PROJECTION_SOURCE_ROOT="$projection_root"
export CT_HOST_PROJECTION_HOSTNAME=instance-test-host
export CT_HOST_PROJECTION_EXECUTABLE="$fake/apptainer"
export CT_HOST_PROJECTION_BOOT_ID=instance-test-boot
CT_HOST_PROJECTION_GROUPS="$(id -G)"
export CT_HOST_PROJECTION_GROUPS
# shellcheck disable=SC1090
. "$(dirname "$helper")/ct_library.sh"
ct_host_projection_selection_key apptainer "$image_path" '' native-inherited
selection_key=$CT_HOST_PROJECTION_SELECTION_KEY
CT_HOST_PROJECTION_SOURCES=("$bind_source")
CT_HOST_PROJECTION_TARGETS=("$(realpath "$bind_source")")
ct_host_projection_cache_write "$selection_key" fallback complete native-inherited proven

contains_line() {
  local file=$1
  local expected=$2
  local line
  while IFS= read -r line; do
    [[ $line != "$expected" ]] || return 0
  done < "$file"
  return 1
}

invoke() {
  "$helper" --apptainer --ct-instance-root "$instance_root" \
    --ct-bind "$bind_source:$work/container bind" \
    --ct-env 'TEST_VALUE=space value' \
    --ct-bootstrap "$bootstrap" \
    -- "$image_path" /bin/fake-command 'literal argument with spaces' '' '--leading-dash' '*'
}

set +e
invoke
first_status=$?
set -e
[[ $first_status -eq 23 ]] || { printf '%s\n' 'instance executor did not preserve payload status' >&2; exit 1; }
[[ $(<"$work/call-count") -eq 4 ]] || { printf '%s\n' 'first invocation did not probe, start, verify, and execute' >&2; exit 1; }
[[ -d $instance_root && ! -L $instance_root && $(stat -Lc '%a' -- "$instance_root") == 700 ]] || {
  printf '%s\n' 'first invocation did not create a private instance root' >&2; exit 1;
}
mapfile -t start_call < "$calls/2"
[[ ${start_call[0]} == instance && ${start_call[1]} == start ]] || { printf '%s\n' 'instance start command is missing' >&2; exit 1; }
name=${start_call[-1]}
[[ $name =~ ^mkchad-[0-9a-f]{32}$ && ${start_call[-2]} == "$image_path" ]] || { printf '%s\n' 'instance profile name or image changed' >&2; exit 1; }
contains_line "$calls/2" "$bind_source:$work/container bind" || { printf '%s\n' 'instance start omitted the literal bind' >&2; exit 1; }
contains_line "$calls/2" "type=bind,src=$bind_source,dst=/host$bind_source" || {
  printf '%s\n' 'instance start omitted the selected generated host bind' >&2; exit 1;
}
start_has_profile=0
while IFS= read -r start_argument; do
  [[ $start_argument != CT_HOST_PROJECTION_PROFILE=* ]] || start_has_profile=1
done < "$calls/2"
if [[ $start_has_profile -ne 1 ]]; then
  printf '%s\n' 'instance start omitted the persistent host-projection profile' >&2
  exit 1
fi
contains_line "$calls/4" "instance://$name" || { printf '%s\n' 'payload did not enter the created instance' >&2; exit 1; }
contains_line "$calls/4" '/.container-tools-bootstrap' || { printf '%s\n' 'payload omitted the bootstrap' >&2; exit 1; }
mapfile -t payload < "$calls/4"
uri_index=0
for index in "${!payload[@]}"; do
  [[ ${payload[index]} != "instance://$name" ]] || uri_index=$index
done
expected_tail=(/.container-tools-bootstrap /bin/fake-command 'literal argument with spaces' '' '--leading-dash' '*')
actual_tail=("${payload[@]:uri_index+1}")
[[ ${#actual_tail[@]} -eq ${#expected_tail[@]} ]] || { printf '%s\n' 'payload argument count changed' >&2; exit 1; }
for index in "${!expected_tail[@]}"; do
  [[ ${actual_tail[index]} == "${expected_tail[index]}" ]] || { printf '%s\n' 'payload argument order changed' >&2; exit 1; }
done
if contains_line "$calls/4" --bind || contains_line "$calls/4" --mount; then
  printf '%s\n' 'instance exec attempted to change fixed bind mounts' >&2
  exit 1
fi

set +e
invoke
second_status=$?
set -e
[[ $second_status -eq 23 && $(<"$work/call-count") -eq 6 ]] || {
  printf '%s\n' 'second invocation did not reuse the existing instance' >&2; exit 1;
}
contains_line "$calls/6" "instance://$name" || { printf '%s\n' 'second invocation selected another instance' >&2; exit 1; }

set +e
APPTAINER_BIND=/untrusted APPTAINER_MOUNT=/untrusted invoke
inherited_mount_status=$?
set -e
[[ $inherited_mount_status -eq 23 && $(<"$work/call-count") -eq 8 ]] || {
  printf '%s\n' 'inherited runtime mounts altered instance reuse' >&2; exit 1;
}

sleep 1
printf 'rotated\n' > "$image_path"
set +e
invoke
rotated_status=$?
set -e
[[ $rotated_status -eq 23 && $(<"$work/call-count") -eq 12 ]] || {
  printf '%s\n' 'rotated image did not establish a new instance profile' >&2; exit 1;
}
mapfile -t rotated_start < "$calls/10"
[[ ${rotated_start[0]} == instance && ${rotated_start[1]} == start && ${rotated_start[-1]} != "$name" ]] || {
  printf '%s\n' 'rotated image reused the stale instance profile' >&2; exit 1;
}

second_bind="$work/second bind"
mkdir "$second_bind"
invoke_concurrent() {
  "$helper" --apptainer --ct-instance-root "$instance_root" \
    --ct-bind "$second_bind:$work/second container bind" \
    --ct-env 'TEST_VALUE=space value' \
    --ct-bootstrap "$bootstrap" \
    -- "$image_path" /bin/fake-command concurrent
}
export MKCHAD_TEST_START_DELAY=0.2
set +e
invoke_concurrent &
first_pid=$!
invoke_concurrent &
second_pid=$!
wait "$first_pid"; first_concurrent=$?
wait "$second_pid"; second_concurrent=$?
set -e
[[ $first_concurrent -eq 23 && $second_concurrent -eq 23 ]] || {
  printf '%s\n' 'concurrent callers did not both reach the shared instance' >&2; exit 1;
}
starts=0
for call in "$calls"/*; do
  mapfile -t argv < "$call"
  if [[ ${argv[0]:-} == instance && ${argv[1]:-} == start ]]; then
    starts=$((starts + 1))
  fi
done
[[ $starts -eq 3 ]] || { printf '%s\n' 'concurrent first use started more than one profile instance' >&2; exit 1; }

cp "$fake/apptainer" "$fake/singularity"
set +e
"$helper" --singularity --ct-instance-root "$instance_root" \
  --ct-bind "$bind_source:$work/container bind" \
  --ct-env 'TEST_VALUE=space value' \
  --ct-bootstrap "$bootstrap" \
  -- "$image_path" /bin/fake-command singularity
singularity_status=$?
set -e
[[ $singularity_status -eq 23 ]] || { printf '%s\n' 'Singularity instance flow changed' >&2; exit 1; }

old_bind="$work/old bind"
mv "$bind_source" "$old_bind"
mkdir "$bind_source"
set +e
invoke
replaced_bind_status=$?
set -e
[[ $replaced_bind_status -eq 23 ]] || { printf '%s\n' 'replaced bind source did not remain executable' >&2; exit 1; }

other_root="$work/other instance root"
set +e
"$helper" --apptainer --ct-instance-root "$other_root" \
  --ct-bind "$bind_source:$work/container bind" \
  --ct-env 'TEST_VALUE=space value' \
  --ct-bootstrap "$bootstrap" \
  -- "$image_path" /bin/fake-command other-root
other_root_status=$?
set -e
[[ $other_root_status -eq 23 ]] || { printf '%s\n' 'second private root did not receive an isolated instance profile' >&2; exit 1; }

printf '# rotated bootstrap\n' >> "$bootstrap"
set +e
invoke
rotated_bootstrap_status=$?
set -e
[[ $rotated_bootstrap_status -eq 23 ]] || { printf '%s\n' 'rotated bootstrap did not establish a new instance profile' >&2; exit 1; }

latest_call=$(<"$work/call-count")
mapfile -t latest_payload < "$calls/$latest_call"
latest_name=
for argument in "${latest_payload[@]}"; do
  [[ $argument != instance://* ]] || latest_name=${argument#instance://}
done
lock_marker="$work/lock-held"
(
  exec 7>"$instance_root/$latest_name.lock"
  flock 7
  : > "$lock_marker"
  sleep 10
) &
lock_holder=$!
for _ in {1..100}; do
  [[ ! -e $lock_marker ]] || break
  sleep 0.01
done
[[ -e $lock_marker ]] || { printf '%s\n' 'test lock holder did not acquire the profile lock' >&2; exit 1; }
SECONDS=0
set +e
CT_INSTANCE_LOCK_TIMEOUT=0.1 invoke >"$work/lock-timeout.out" 2>"$work/lock-timeout.err"
lock_timeout_status=$?
kill "$lock_holder"
wait "$lock_holder" 2>/dev/null
set -e
[[ $lock_timeout_status -eq 1 && $SECONDS -lt 3 \
  && $(<"$work/lock-timeout.err") == *'timed out waiting for persistent instance creation'* ]] || {
  printf '%s\n' 'profile lock acquisition was not bounded' >&2; exit 1;
}

uncovered_cwd="$work/uncovered cwd"
mkdir "$uncovered_cwd"
set +e
(
  cd "$uncovered_cwd"
  "$helper" --apptainer --ct-instance-root "$instance_root" \
    --ct-bind "$bind_source:$work/container bind" \
    --ct-bootstrap "$bootstrap" \
    -- "$image_path" /bin/fake-command uncovered-cwd
)
uncovered_cwd_status=$?
set -e
[[ $uncovered_cwd_status -eq 23 ]] || { printf '%s\n' 'uncovered working directory was not preserved' >&2; exit 1; }
latest_call=$(<"$work/call-count")
contains_line "$calls/$((latest_call - 2))" "$uncovered_cwd:$uncovered_cwd" || {
  printf '%s\n' 'instance start omitted the uncovered working-directory bind' >&2; exit 1;
}

starts=0
for call in "$calls"/*; do
  mapfile -t argv < "$call"
  if [[ ${argv[0]:-} == instance && ${argv[1]:-} == start ]]; then
    starts=$((starts + 1))
  fi
done
[[ $starts -eq 8 ]] || { printf '%s\n' 'profile inputs did not select exactly one instance each' >&2; exit 1; }

SECONDS=0
set +e
MKCHAD_TEST_HANG_PROBE=1 CT_INSTANCE_PROBE_TIMEOUT=0.1 CT_INSTANCE_START_TIMEOUT=0.1 invoke \
  >"$work/probe-timeout.out" 2>"$work/probe-timeout.err"
probe_timeout_status=$?
set -e
[[ $probe_timeout_status -eq 1 && $SECONDS -lt 3 ]] || {
  printf '%s\n' 'hanging instance probe was not bounded' >&2; exit 1;
}

timeout_root="$work/timeout instance root"
timeout_bind="$work/timeout bind"
mkdir "$timeout_bind"
SECONDS=0
set +e
MKCHAD_TEST_HANG_START=1 CT_INSTANCE_PROBE_TIMEOUT=0.1 CT_INSTANCE_START_TIMEOUT=0.1 \
  "$helper" --apptainer --ct-instance-root "$timeout_root" \
    --ct-bind "$timeout_bind:$work/timeout container bind" \
    -- "$image_path" /bin/fake-command timeout \
    >"$work/start-timeout.out" 2>"$work/start-timeout.err"
start_timeout_status=$?
set -e
[[ $start_timeout_status -eq 1 && $SECONDS -lt 3 ]] || {
  printf '%s\n' 'hanging instance start was not bounded' >&2; exit 1;
}

mutation_marker="$work/mutated-during-probe"
set +e
MKCHAD_TEST_MUTATE_ON_PROBE="$image_path" MKCHAD_TEST_MUTATE_MARKER="$mutation_marker" invoke \
  >"$work/asset-race.out" 2>"$work/asset-race.err"
asset_race_status=$?
set -e
[[ $asset_race_status -eq 1 && -e $mutation_marker \
  && $(<"$work/asset-race.err") == *'container image or bootstrap changed while preparing its instance'* ]] || {
  printf '%s\n' 'asset mutation during probe did not fail closed' >&2; exit 1;
}

# Effective supplementary-group changes select another instance without
# changing the already memoized host-projection capability selection.
invoke_host_root_mode() {
  "$helper" --apptainer --ct-instance-root "$instance_root" \
    --ct-host-root "$1" \
    --ct-bind "$bind_source:$work/container bind" \
    --ct-env 'TEST_VALUE=space value' \
    --ct-bootstrap "$bootstrap" \
    -- "$image_path" /bin/fake-command host-root-mode
}
invoke_host_root_refresh() {
  "$helper" --apptainer --ct-instance-root "$instance_root" \
    --ct-host-root auto --ct-host-root-refresh \
    --ct-bind "$bind_source:$work/container bind" \
    --ct-env 'TEST_VALUE=space value' \
    --ct-bootstrap "$bootstrap" \
    -- "$image_path" /bin/fake-command host-root-refresh
}
set +e
invoke
matching_host_root_status=$?
set -e
[[ $matching_host_root_status -eq 23 ]] || {
  printf '%s\n' 'host-root mode fixture did not establish its matching profile' >&2; exit 1;
}
for host_root_case in required refresh; do
  calls_before=$(<"$work/call-count")
  set +e
  if [[ $host_root_case == required ]]; then
    invoke_host_root_mode required
  else
    invoke_host_root_refresh
  fi
  host_root_status=$?
  set -e
  [[ $host_root_status -eq 23 && $(<"$work/call-count") -eq $((calls_before + 2)) ]] || {
    printf '%s\n' "host-root $host_root_case did not reuse the matching persistent profile" >&2; exit 1;
  }
done

original_groups=$MKCHAD_TEST_GROUPS
export MKCHAD_TEST_GROUPS="$primary_group 424242"
calls_before=$(<"$work/call-count")
set +e
invoke
group_status=$?
set -e
[[ $group_status -eq 23 && $(<"$work/call-count") -eq $((calls_before + 4)) ]] || {
  printf '%s\n' 'supplementary-group realization did not select a fresh profile' >&2; exit 1;
}
export MKCHAD_TEST_GROUPS=$original_groups

# An additional explicit bind changes the full instance profile but does not
# require another projection capability operation because the selection record
# has no explicit-bind material in its key.
extra_bind="$work/extra bind"
mkdir "$extra_bind"
invoke_extra_bind() {
  "$helper" --apptainer --ct-instance-root "$instance_root" \
    --ct-bind "$bind_source:$work/container bind" \
    --ct-bind "$extra_bind:$work/extra container bind" \
    --ct-bootstrap "$bootstrap" \
    -- "$image_path" /bin/fake-command explicit-bind
}
calls_before=$(<"$work/call-count")
set +e
invoke_extra_bind
extra_bind_status=$?
set -e
[[ $extra_bind_status -eq 23 && $(<"$work/call-count") -eq $((calls_before + 4)) ]] || {
  printf '%s\n' 'explicit bind changed projection selection or reused its old profile' >&2; exit 1;
}

# A reported profile mismatch is terminal: it performs one liveness operation
# and cannot start, stop, or dispatch through an existing instance.
set +e
invoke
matching_profile_status=$?
set -e
[[ $matching_profile_status -eq 23 ]] || {
  printf '%s\n' 'profile-mismatch fixture did not create its matching instance' >&2; exit 1;
}
calls_before=$(<"$work/call-count")
set +e
MKCHAD_TEST_FORCE_PROFILE_MISMATCH=1 invoke >"$work/profile-mismatch.out" 2>"$work/profile-mismatch.err"
mismatch_status=$?
set -e
[[ $mismatch_status -eq 1 && $(<"$work/call-count") -eq $((calls_before + 1)) \
  && $(<"$work/profile-mismatch.err") == *'persistent instance profile mismatch'* ]] || {
  printf '%s\n' 'profile mismatch was not terminal before payload dispatch' >&2; exit 1;
}

# A dry run returns the rendered command without creating either persistent
# instance state or a projection cache/pending record.
dry_root="$work/dry instance root"
dry_cache="$work/dry projection cache"
CT_DRY_RUN=1 CT_HOST_PROJECTION_CACHE_ROOT="$dry_cache" "$helper" --apptainer \
  --ct-instance-root "$dry_root" -- "$image_path" /bin/fake-command dry >"$work/dry.out"
[[ ! -e $dry_root && ! -e $dry_cache && $(<"$work/dry.out") == *'instance://dry-run'* ]] || {
  printf '%s\n' 'persistent dry run mutated runtime state' >&2; exit 1;
}

# Interrupted creation leaves a mode-0600 nonce journal. A later caller adopts
# only a live instance carrying that exact nonce; a mismatched nonce is left
# untouched and never triggers a stop operation.
recovery_root="$work/recovery instance root"
recovery_bind="$work/recovery bind"
recovery_image="$work/recovery.sif"
mkdir "$recovery_bind"
: > "$recovery_image"
recovery_invoke() {
  "$helper" --apptainer --ct-instance-root "$recovery_root" \
    --ct-bind "$recovery_bind:$work/recovery container bind" \
    -- "$recovery_image" /bin/fake-command recovery
}
set +e
CT_INSTANCE_CREATION_NONCE=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa MKCHAD_TEST_HANG_START=1 \
  CT_INSTANCE_START_TIMEOUT=0.1 recovery_invoke >"$work/recovery-timeout.out" 2>"$work/recovery-timeout.err"
recovery_timeout_status=$?
set -e
pending_files=("$recovery_root"/*.pending)
pending_file=${pending_files[0]:-}
[[ $recovery_timeout_status -eq 1 && -n $pending_file && -e $pending_file \
  && $(stat -Lc '%a' -- "$pending_file") == 600 ]] || {
  printf '%s\n' 'interrupted creation did not write a private pending journal' >&2; exit 1;
}
mapfile -t pending_fields < "$pending_file"
recovery_name=${pending_fields[0]}
: > "$instances/$recovery_name"
printf '%s\n' "${pending_fields[1]}" > "$instances/$recovery_name.profile"
printf '%s\n' "${pending_fields[2]}" > "$instances/$recovery_name.nonce"
calls_before=$(<"$work/call-count")
set +e
recovery_invoke
recovery_status=$?
set -e
[[ $recovery_status -eq 23 && ! -e $pending_file && $(<"$work/call-count") -eq $((calls_before + 2)) ]] || {
  printf '%s\n' 'matching pending creation was not adopted with one liveness and one payload' >&2; exit 1;
}

mismatch_recovery_root="$work/mismatch recovery root"
mismatch_recovery_image="$work/mismatch-recovery.sif"
: > "$mismatch_recovery_image"
mismatch_recovery_invoke() {
  "$helper" --apptainer --ct-instance-root "$mismatch_recovery_root" \
    --ct-bind "$recovery_bind:$work/mismatch recovery container bind" \
    -- "$mismatch_recovery_image" /bin/fake-command recovery-mismatch
}
set +e
CT_INSTANCE_CREATION_NONCE=cccccccccccccccccccccccccccccccc MKCHAD_TEST_HANG_START=1 \
  CT_INSTANCE_START_TIMEOUT=0.1 mismatch_recovery_invoke >/dev/null 2>&1
mismatch_timeout_status=$?
set -e
mismatch_pending_files=("$mismatch_recovery_root"/*.pending)
mismatch_pending=${mismatch_pending_files[0]:-}
[[ $mismatch_timeout_status -eq 1 && -n $mismatch_pending && -e $mismatch_pending ]] || {
  printf '%s\n' 'mismatched-nonce fixture did not retain its pending journal' >&2; exit 1;
}
mapfile -t mismatch_pending_fields < "$mismatch_pending"
mismatch_name=${mismatch_pending_fields[0]}
: > "$instances/$mismatch_name"
printf '%s\n' "${mismatch_pending_fields[1]}" > "$instances/$mismatch_name.profile"
printf '%s\n' bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb > "$instances/$mismatch_name.nonce"
calls_before=$(<"$work/call-count")
set +e
mismatch_recovery_invoke >"$work/mismatch-recovery.out" 2>"$work/mismatch-recovery.err"
mismatch_recovery_status=$?
set -e
[[ $mismatch_recovery_status -eq 1 && -e $mismatch_pending \
  && $(<"$work/call-count") -eq $((calls_before + 2)) \
  && $(<"$work/mismatch-recovery.err") == *'pending nonce mismatch'* ]] || {
  printf '%s\n' 'pending recovery accepted or cleaned a mismatched nonce' >&2; exit 1;
}

printf '%s\n' 'container instance executor tests passed'
