#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

work=${1:?pass a task-specific directory beneath /tmp/mkchad-v1/container-tools-c11}
helper=${2:?pass the ct_instance_exec.sh path}
helper=$(realpath "$helper")
work=$(realpath -m -- "$work")
[[ $work == /tmp/mkchad-v1/container-tools-c11/* ]] || { printf '%s\n' 'test directory must be beneath /tmp/mkchad-v1/container-tools-c11' >&2; exit 2; }
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
if [[ ! ( ${MKCHAD_TEST_HIDE_INSTANCE_LIST:-} == 1 && ${1:-} == instance && ${2:-} == list ) ]]; then
  exec 8>"$MKCHAD_TEST_CALL_LOCK"
  flock 8
  count=0
  [[ ! -f $MKCHAD_TEST_CALL_COUNT ]] || read -r count < "$MKCHAD_TEST_CALL_COUNT"
  count=$((count + 1))
  printf '%s\n' "$count" > "$MKCHAD_TEST_CALL_COUNT"
  printf '%s\n' "$@" > "$MKCHAD_TEST_CALLS/$count"
  flock -u 8
fi

if [[ ${1:-} == instance && ${2:-} == start ]]; then
  args=("$@")
  for argument in "$@"; do
    [[ $argument != --pwd ]] || exit 67
  done
  name=${!#}
  identity_source=
  mount_plan_source=
  for ((index = 0; index < ${#args[@]} - 1; index++)); do
    if [[ ${args[index]} == --bind && ${args[index + 1]} == *:/.container-tools-instance-identity:ro ]]; then
      identity_source=${args[index + 1]%:/.container-tools-instance-identity:ro}
    fi
    if [[ ${args[index]} == --bind && ${args[index + 1]} == *:/.container-tools-mount-plan:ro ]]; then
      mount_plan_source=${args[index + 1]%:/.container-tools-mount-plan:ro}
    fi
  done
  mapfile -t identity_fields 2>/dev/null < "$identity_source" || exit 68
  [[ ${#identity_fields[@]} == 3 && ${identity_fields[0]} == "$name" \
    && ${identity_fields[1]} =~ ^[0-9a-f]{64}$ && ${identity_fields[2]} =~ ^[0-9a-f]{32}$ ]] || exit 68
  [[ -n $mount_plan_source && -f $mount_plan_source && ! -L $mount_plan_source \
    && $(stat -c '%a' -- "$mount_plan_source") == 600 ]] || exit 68
  mapfile -d '' -t mount_plan_fields < "$mount_plan_source" || exit 68
  [[ ${mount_plan_fields[0]} == ct-mount-plan-v1 && ${mount_plan_fields[1]} =~ ^[0-9a-f]{64}$ \
    && ( ${mount_plan_fields[2]} == apptainer || ${mount_plan_fields[2]} == singularity ) \
    && ${mount_plan_fields[6]} =~ ^[0-9]+$ ]] || exit 68
  (( ${#mount_plan_fields[@]} == 7 + 5 * 10#${mount_plan_fields[6]} )) || exit 68
  mount_plan_digest=$({ printf '%s\0' "${mount_plan_fields[0]}"; printf '%s\0' "${mount_plan_fields[@]:2}"; } | sha256sum)
  mount_plan_digest=${mount_plan_digest%% *}
  [[ $mount_plan_digest == "${mount_plan_fields[1]}" \
    && ${mount_plan_source##*/} == "$mount_plan_digest.manifest" ]] || exit 68
  mount_plan_bytes=0
  for mount_plan_field in "${mount_plan_fields[@]}"; do
    mount_plan_bytes=$((mount_plan_bytes + ${#mount_plan_field} + 1))
  done
  (( mount_plan_bytes == $(stat -c '%s' -- "$mount_plan_source") )) || exit 68
  [[ ${MKCHAD_TEST_HANG_START:-} != 1 ]] || sleep 10
  sleep "${MKCHAD_TEST_START_DELAY:-0}"
  [[ ! -e $MKCHAD_TEST_INSTANCES/$name ]] || exit 42
  : > "$MKCHAD_TEST_INSTANCES/$name"
  printf '%s\n' "${identity_fields[1]}" > "$MKCHAD_TEST_INSTANCES/$name.profile"
  printf '%s\n' "${identity_fields[2]}" > "$MKCHAD_TEST_INSTANCES/$name.nonce"
  exit 0
fi

if [[ ${1:-} == instance && ${2:-} == list && ${3:-} == --json ]]; then
  name=${4:-}
  [[ $name =~ ^mkchad-[0-9a-f]{32}$ ]] || exit 69
  [[ ${MKCHAD_TEST_INSTANCE_LIST_FAIL:-} != 1 ]] || exit 9
  if [[ ${MKCHAD_TEST_DELAY_OLD_ON_LIST:-} == 1 && ! -e ${MKCHAD_TEST_DELAY_OLD_MARKER:-} ]]; then
    : > "$MKCHAD_TEST_DELAY_OLD_MARKER"
    printf '{"instances":[]}\n'
    : > "$MKCHAD_TEST_INSTANCES/$name"
    printf '%s\n' "$MKCHAD_TEST_DELAY_OLD_PROFILE" > "$MKCHAD_TEST_INSTANCES/$name.profile"
    printf '%s\n' "$MKCHAD_TEST_DELAY_OLD_NONCE" > "$MKCHAD_TEST_INSTANCES/$name.nonce"
    exit 0
  fi
  if [[ -e $MKCHAD_TEST_INSTANCES/$name ]]; then
    printf '{"instances":[{"instance":"%s"}]}\n' "$name"
  else
    printf '{"instances":[]}\n'
  fi
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
    if [[ ${MKCHAD_TEST_HANG_PROBE_ONCE:-} == 1 && ! -e ${MKCHAD_TEST_HANG_PROBE_MARKER:-} ]]; then
      : > "$MKCHAD_TEST_HANG_PROBE_MARKER"
      sleep 10
    fi
    [[ ${MKCHAD_TEST_FAIL_PROBE:-} != 1 ]] || exit 9
    if [[ -n ${MKCHAD_TEST_MUTATE_ON_PROBE:-} && ! -e ${MKCHAD_TEST_MUTATE_MARKER:-} ]]; then
      printf 'mutated during probe\n' >> "$MKCHAD_TEST_MUTATE_ON_PROBE"
      : > "$MKCHAD_TEST_MUTATE_MARKER"
    fi
    expected_profile=${@: -2:1}
    expected_nonce=${!#}
    [[ "$*" == *'/.container-tools-instance-identity'* ]] || exit 42
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
printf '%s\n' '--exclude-path /.container-tools-instance-identity' > "$work/mount-config"
export CT_MOUNT_CFG="$work/mount-config"
# Avoid inheriting the enclosing container's identity mount in this fake test.
export MKCHAD_TEST_CALLS="$calls"
export MKCHAD_TEST_CALL_COUNT="$work/call-count"
export MKCHAD_TEST_CALL_LOCK="$work/call.lock"
export MKCHAD_TEST_INSTANCES="$instances"
export MKCHAD_TEST_HIDE_INSTANCE_LIST=1
MKCHAD_TEST_GROUPS="$($real_id -G)"
export MKCHAD_TEST_GROUPS

# Seed the projection selection record so persistent warm-path assertions do
# not need a capability runtime probe. The instance helper must consume this
# record and add the generated bind before finalizing its profile.
mountinfo="$work/mountinfo"
printf '24 1 8:1 / / rw,relatime - ext4 /dev/root rw\n25 24 8:2 / %s rw,relatime - ext4 /dev/test rw\n' "$bind_source" > "$mountinfo"
export CT_HOST_PROJECTION_CACHE_ROOT="$work/projection-cache"
export CT_MOUNT_PLAN_STATE_ROOT="$work/mount-plans"
export CT_HOST_PROJECTION_MOUNTINFO="$mountinfo"
export CT_HOST_PROJECTION_SOURCE_ROOT="$projection_root"
export CT_HOST_PROJECTION_HOSTNAME=instance-test-host
export CT_HOST_PROJECTION_EXECUTABLE="$fake/apptainer"
export CT_HOST_PROJECTION_BOOT_ID=instance-test-boot
CT_HOST_PROJECTION_GROUPS="$(id -G)"
export CT_HOST_PROJECTION_GROUPS
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
  CT_HOST_PROJECTION_GROUPS="$MKCHAD_TEST_GROUPS" "$helper" --apptainer --ct-instance-root "$instance_root" \
    --ct-bind "$bind_source:$work/container bind" \
    --ct-env 'TEST_VALUE=space value' \
    --ct-bootstrap "$bootstrap" \
    -- "$image_path" /bin/fake-command 'literal argument with spaces' '' '--leading-dash' '*'
}

for unsafe_root in "$work/unsafe:root" "$work/unsafe,root"; do
  set +e
  "$helper" --apptainer --ct-instance-root "$unsafe_root" \
    -- "$image_path" /bin/fake-command unsafe-root \
    >"$work/unsafe-root.out" 2>"$work/unsafe-root.err"
  unsafe_root_status=$?
  set -e
  [[ $unsafe_root_status -eq 1 && ! -e $unsafe_root && ! -e $work/call-count \
    && $(<"$work/unsafe-root.err") == *'persistent instance request: usage:'* \
    && $(<"$work/unsafe-root.err") != *"$unsafe_root"* ]] || {
    printf '%s\n' 'delimiter-bearing instance root reached runtime or filesystem mutation' >&2
    exit 1
  }
done

state_setup_root="$work/invalid instance state"
mkdir "$state_setup_root"
chmod 750 "$state_setup_root"
set +e
"$helper" --apptainer --ct-instance-root "$state_setup_root" \
  -- "$image_path" /bin/fake-command invalid-state \
  >"$work/invalid-state.out" 2>"$work/invalid-state.err"
state_setup_status=$?
set -e
[[ $state_setup_status -eq 1 && ! -e $work/call-count \
  && $(<"$work/invalid-state.err") == *'persistent instance state: setup:'* \
  && $(<"$work/invalid-state.err") != *"$state_setup_root"* ]] || {
  printf '%s\n' 'invalid persistent state was not distinguished from lock contention' >&2
  exit 1
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
start_has_identity=0
while IFS= read -r start_argument; do
  [[ $start_argument != "$instance_root/$name.pending:/.container-tools-instance-identity:ro" ]] || start_has_identity=1
done < "$calls/2"
if [[ $start_has_identity -ne 1 ]]; then
  printf '%s\n' 'instance start omitted the pinned persistent identity record' >&2
  exit 1
fi
start_has_mount_plan=0
while IFS= read -r start_argument; do
  [[ $start_argument != *':/.container-tools-mount-plan:ro' ]] || start_has_mount_plan=1
done < "$calls/2"
if [[ $start_has_mount_plan -ne 1 ]]; then
  printf '%s\n' 'instance start omitted the stable mount-plan bind' >&2
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

reserved_source="$work/reserved identity source"
mkdir "$reserved_source"
calls_before=$(<"$work/call-count")
set +e
"$helper" --apptainer --ct-instance-root "$instance_root" \
  --ct-bind "$reserved_source:/.container-tools-instance-identity" \
  -- "$image_path" /bin/fake-command reserved-identity \
  >"$work/reserved-identity.out" 2>"$work/reserved-identity.err"
reserved_identity_status=$?
set -e
[[ $reserved_identity_status -eq 1 && $(<"$work/call-count") -eq "$calls_before" \
  && $(<"$work/reserved-identity.err") == *'persistent instance request: reserved-bind:'* \
  && $(<"$work/reserved-identity.err") != *"$reserved_source"* ]] || {
  printf '%s\n' 'caller bind reached or replaced the reserved instance identity path' >&2
  exit 1
}

calls_before=$(<"$work/call-count")
set +e
"$helper" --apptainer --ct-instance-root "$instance_root" \
  --ct-bind "$reserved_source:/.container-tools-mount-plan" \
  -- "$image_path" /bin/fake-command reserved-mount-plan \
  >"$work/reserved-mount-plan.out" 2>"$work/reserved-mount-plan.err"
reserved_mount_plan_status=$?
set -e
[[ $reserved_mount_plan_status -eq 1 && $(<"$work/call-count") -eq "$calls_before" \
  && $(<"$work/reserved-mount-plan.err") == *'persistent instance request: reserved-bind:'* \
  && $(<"$work/reserved-mount-plan.err") != *"$reserved_source"* ]] || {
  printf '%s\n' 'persistent caller bind reached the reserved mount-plan path' >&2
  exit 1
}

# A transient first liveness error cannot authorize a new pending journal while
# the structured instance list still reports the named instance.
calls_before=$(<"$work/call-count")
unset MKCHAD_TEST_HIDE_INSTANCE_LIST
set +e
MKCHAD_TEST_FAIL_PROBE=1 invoke > /dev/null 2> "$work/transient-first-probe.err"
transient_first_status=$?
set -e
transient_first_pending=("$instance_root"/*.pending)
[[ $transient_first_status -eq 1 && $(<"$work/call-count") -eq $((calls_before + 2)) \
  && ! -e ${transient_first_pending[0]} \
  && $(<"$work/transient-first-probe.err") == *'persistent instance liveness: recovery:'* ]] || {
  printf '%s\n' 'transient first liveness failure created or started conflicting state' >&2
  exit 1
}
rm -f -- "$calls/$((calls_before + 1))" "$calls/$((calls_before + 2))"
printf '%s\n' "$calls_before" > "$work/call-count"
export MKCHAD_TEST_HIDE_INSTANCE_LIST=1

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
  && $(<"$work/lock-timeout.err") == *'persistent instance lock: contention:'* ]] || {
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
cwd_plan=
while IFS= read -r start_argument; do
  [[ $start_argument != *':/.container-tools-mount-plan:ro' ]] || {
    cwd_plan=${start_argument%:/.container-tools-mount-plan:ro}
    break
  }
done < "$calls/$((latest_call - 2))"
[[ -n $cwd_plan ]] || { printf '%s\n' 'persistent start omitted its cwd mount-plan source' >&2; exit 1; }
mapfile -d '' -t cwd_plan_fields < "$cwd_plan"
cwd_plan_has_entry=0
for ((field_index = 7; field_index < ${#cwd_plan_fields[@]}; field_index += 5)); do
  [[ ${cwd_plan_fields[field_index]} == persistent-automatic-cwd \
    && ${cwd_plan_fields[field_index + 1]} == "$uncovered_cwd" \
    && ${cwd_plan_fields[field_index + 2]} == "$uncovered_cwd" \
    && ${cwd_plan_fields[field_index + 3]} == inherit \
    && ${cwd_plan_fields[field_index + 4]} == runtime-default ]] && cwd_plan_has_entry=1
done
(( cwd_plan_has_entry )) || {
  printf '%s\n' 'persistent working-directory bind was absent from the semantic mount plan' >&2
  exit 1
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
[[ $(<"$work/probe-timeout.err") == *'persistent instance liveness: timeout:'* ]] || {
  printf '%s\n' 'probe timeout did not provide a redacted recovery diagnostic' >&2; exit 1;
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
[[ $(<"$work/start-timeout.err") == *'persistent instance backend-start: timeout:'* ]] || {
  printf '%s\n' 'start timeout did not provide a redacted recovery diagnostic' >&2; exit 1;
}

mutation_marker="$work/mutated-during-probe"
set +e
MKCHAD_TEST_MUTATE_ON_PROBE="$image_path" MKCHAD_TEST_MUTATE_MARKER="$mutation_marker" invoke \
  >"$work/asset-race.out" 2>"$work/asset-race.err"
asset_race_status=$?
set -e
[[ $asset_race_status -eq 1 && -e $mutation_marker \
  && $(<"$work/asset-race.err") == *'persistent instance assets: changed:'* \
  && $(<"$work/asset-race.err") != *"$image_path"* ]] || {
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
  && $(<"$work/profile-mismatch.err") == *'persistent instance recovery: profile-mismatch:'* ]] || {
  printf '%s\n' 'profile mismatch was not terminal before payload dispatch' >&2; exit 1;
}

# A dry run returns the rendered command without creating either persistent
# instance state or a projection cache/pending record.
dry_root="$work/dry instance root"
dry_cache="$work/dry projection cache"
dry_mount_plans="$work/dry mount plans"
CT_DRY_RUN=1 CT_HOST_PROJECTION_CACHE_ROOT="$dry_cache" CT_MOUNT_PLAN_STATE_ROOT="$dry_mount_plans" "$helper" --apptainer \
  --ct-instance-root "$dry_root" -- "$image_path" /bin/fake-command dry >"$work/dry.out"
[[ ! -e $dry_root && ! -e $dry_cache && ! -e $dry_mount_plans \
  && $(<"$work/dry.out") == *'instance://dry-run'* ]] || {
  printf '%s\n' 'persistent dry run mutated runtime state' >&2; exit 1;
}

# Interrupted creation leaves a mode-0600 nonce journal. A later caller adopts
# only a live instance carrying that exact nonce; a mismatched nonce is left
# untouched and never triggers a stop operation.
unset MKCHAD_TEST_HIDE_INSTANCE_LIST
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
  && $(<"$work/mismatch-recovery.err") == *'persistent instance recovery: pending-nonce-mismatch:'* \
  && $(<"$work/mismatch-recovery.err") != *"${mismatch_pending_fields[2]}"* ]] || {
  printf '%s\n' 'pending recovery accepted or cleaned a mismatched nonce' >&2; exit 1;
}

# A pending nonce stays authoritative through an ambiguous liveness timeout. A
# listed response preserves it until a later exact probe can adopt it.
timeout_recovery_root="$work/timeout recovery root"
timeout_recovery_image="$work/timeout-recovery.sif"
: > "$timeout_recovery_image"
timeout_recovery_invoke() {
  "$helper" --apptainer --ct-instance-root "$timeout_recovery_root" \
    --ct-bind "$recovery_bind:$work/timeout recovery container bind" \
    -- "$timeout_recovery_image" /bin/fake-command recovery-timeout
}
set +e
CT_INSTANCE_CREATION_NONCE=dddddddddddddddddddddddddddddddd MKCHAD_TEST_HANG_START=1 \
  CT_INSTANCE_START_TIMEOUT=0.1 timeout_recovery_invoke >/dev/null 2>&1
timeout_creation_status=$?
set -e
timeout_pending_files=("$timeout_recovery_root"/*.pending)
timeout_pending=${timeout_pending_files[0]:-}
[[ $timeout_creation_status -eq 1 && -n $timeout_pending && -e $timeout_pending ]] || {
  printf '%s\n' 'timeout-recovery fixture did not retain its pending journal' >&2; exit 1;
}
mapfile -t timeout_pending_fields < "$timeout_pending"
: > "$instances/${timeout_pending_fields[0]}"
printf '%s\n' "${timeout_pending_fields[1]}" > "$instances/${timeout_pending_fields[0]}.profile"
printf '%s\n' "${timeout_pending_fields[2]}" > "$instances/${timeout_pending_fields[0]}.nonce"
calls_before=$(<"$work/call-count")
timeout_probe_marker="$work/timeout-probe-marker"
set +e
MKCHAD_TEST_HANG_PROBE_ONCE=1 MKCHAD_TEST_HANG_PROBE_MARKER="$timeout_probe_marker" \
  CT_INSTANCE_PROBE_TIMEOUT=0.1 timeout_recovery_invoke >/dev/null 2>&1
timeout_recovery_status=$?
set -e
mapfile -t timeout_list_call < "$calls/$((calls_before + 2))"
[[ $timeout_recovery_status -eq 1 && -e $timeout_pending \
  && ${timeout_list_call[*]} == "instance list --json ${timeout_pending_fields[0]}" \
  && $(<"$work/call-count") -eq $((calls_before + 2)) ]] || {
  printf '%s\n' 'pending timeout did not retain its exact nonce authority' >&2; exit 1;
}
calls_before=$(<"$work/call-count")
set +e
timeout_recovery_invoke >/dev/null 2>&1
timeout_retry_status=$?
set -e
[[ $timeout_retry_status -eq 23 && ! -e $timeout_pending \
  && $(<"$work/call-count") -eq $((calls_before + 2)) ]] || {
  printf '%s\n' 'responsive retry did not adopt the exact pending nonce' >&2; exit 1;
}

# A transient liveness/list failure is not absence. It leaves the original
# journal untouched and must not start, adopt, or dispatch anything.
transient_recovery_root="$work/transient recovery root"
transient_recovery_image="$work/transient-recovery.sif"
: > "$transient_recovery_image"
transient_recovery_invoke() {
  "$helper" --apptainer --ct-instance-root "$transient_recovery_root" \
    --ct-bind "$recovery_bind:$work/transient recovery container bind" \
    -- "$transient_recovery_image" /bin/fake-command recovery-transient
}
set +e
CT_INSTANCE_CREATION_NONCE=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee MKCHAD_TEST_HANG_START=1 \
  CT_INSTANCE_START_TIMEOUT=0.1 transient_recovery_invoke >/dev/null 2>&1
transient_creation_status=$?
set -e
transient_pending_files=("$transient_recovery_root"/*.pending)
transient_pending=${transient_pending_files[0]:-}
[[ $transient_creation_status -eq 1 && -n $transient_pending && -e $transient_pending ]] || {
  printf '%s\n' 'transient-recovery fixture did not retain its pending journal' >&2; exit 1;
}
transient_pending_contents=$(<"$transient_pending")
calls_before=$(<"$work/call-count")
set +e
MKCHAD_TEST_FAIL_PROBE=1 MKCHAD_TEST_INSTANCE_LIST_FAIL=1 transient_recovery_invoke \
  >"$work/transient-recovery.out" 2>"$work/transient-recovery.err"
transient_recovery_status=$?
set -e
[[ $transient_recovery_status -eq 1 && -e $transient_pending \
  && $(<"$transient_pending") == "$transient_pending_contents" \
  && $(<"$work/call-count") -eq $((calls_before + 2)) \
  && $(<"$work/transient-recovery.err") == *'persistent instance recovery: pending-journal-recovery:'* ]] || {
  printf '%s\n' 'transient pending recovery discarded nonce authority or continued' >&2; exit 1;
}

# A delayed creator can appear immediately after a conclusive absence response.
# The retry must retain the old nonce, so its start collision is followed by an
# exact-nonce adoption rather than profile-only reuse or a new nonce.
delayed_recovery_root="$work/delayed recovery root"
delayed_recovery_image="$work/delayed-recovery.sif"
: > "$delayed_recovery_image"
delayed_recovery_invoke() {
  "$helper" --apptainer --ct-instance-root "$delayed_recovery_root" \
    --ct-bind "$recovery_bind:$work/delayed recovery container bind" \
    -- "$delayed_recovery_image" /bin/fake-command recovery-delayed
}
set +e
CT_INSTANCE_CREATION_NONCE=ffffffffffffffffffffffffffffffff MKCHAD_TEST_HANG_START=1 \
  CT_INSTANCE_START_TIMEOUT=0.1 delayed_recovery_invoke >/dev/null 2>&1
delayed_creation_status=$?
set -e
delayed_pending_files=("$delayed_recovery_root"/*.pending)
delayed_pending=${delayed_pending_files[0]:-}
[[ $delayed_creation_status -eq 1 && -n $delayed_pending && -e $delayed_pending ]] || {
  printf '%s\n' 'delayed-recovery fixture did not retain its pending journal' >&2; exit 1;
}
mapfile -t delayed_pending_fields < "$delayed_pending"
calls_before=$(<"$work/call-count")
delayed_marker="$work/delayed-old-marker"
set +e
MKCHAD_TEST_DELAY_OLD_ON_LIST=1 MKCHAD_TEST_DELAY_OLD_MARKER="$delayed_marker" \
  MKCHAD_TEST_DELAY_OLD_PROFILE="${delayed_pending_fields[1]}" \
  MKCHAD_TEST_DELAY_OLD_NONCE="${delayed_pending_fields[2]}" delayed_recovery_invoke >/dev/null 2>&1
delayed_recovery_status=$?
set -e
mapfile -t delayed_list_call < "$calls/$((calls_before + 2))"
mapfile -t delayed_start_call < "$calls/$((calls_before + 3))"
[[ $delayed_recovery_status -eq 23 && -e $delayed_marker && ! -e $delayed_pending \
  && ${delayed_list_call[*]} == "instance list --json ${delayed_pending_fields[0]}" \
  && ${delayed_start_call[*]} == *"$delayed_pending:/.container-tools-instance-identity:ro"* \
  && $(<"$work/call-count") -eq $((calls_before + 5)) ]] || {
  printf '%s\n' 'delayed pending creation was not recovered with its exact nonce' >&2; exit 1;
}

# A clean empty JSON list is the only absence result that may restart a pending
# creation; the retry remains bound to the old nonce and clears its journal only
# after the exact probe succeeds.
absent_recovery_root="$work/absent recovery root"
absent_recovery_image="$work/absent-recovery.sif"
: > "$absent_recovery_image"
absent_recovery_invoke() {
  "$helper" --apptainer --ct-instance-root "$absent_recovery_root" \
    --ct-bind "$recovery_bind:$work/absent recovery container bind" \
    -- "$absent_recovery_image" /bin/fake-command recovery-absent
}
set +e
CT_INSTANCE_CREATION_NONCE=11111111111111111111111111111111 MKCHAD_TEST_HANG_START=1 \
  CT_INSTANCE_START_TIMEOUT=0.1 absent_recovery_invoke >/dev/null 2>&1
absent_creation_status=$?
set -e
absent_pending_files=("$absent_recovery_root"/*.pending)
absent_pending=${absent_pending_files[0]:-}
[[ $absent_creation_status -eq 1 && -n $absent_pending && -e $absent_pending ]] || {
  printf '%s\n' 'absence-recovery fixture did not retain its pending journal' >&2; exit 1;
}
mapfile -t absent_pending_fields < "$absent_pending"
calls_before=$(<"$work/call-count")
set +e
absent_recovery_invoke >/dev/null 2>&1
absent_recovery_status=$?
set -e
mapfile -t absent_list_call < "$calls/$((calls_before + 2))"
mapfile -t absent_start_call < "$calls/$((calls_before + 3))"
[[ $absent_recovery_status -eq 23 && ! -e $absent_pending \
  && ${absent_list_call[*]} == "instance list --json ${absent_pending_fields[0]}" \
  && ${absent_start_call[*]} == *"$absent_pending:/.container-tools-instance-identity:ro"* \
  && $(<"$work/call-count") -eq $((calls_before + 5)) ]] || {
  printf '%s\n' 'conclusive absence did not safely restart with the pending nonce' >&2; exit 1;
}

# Malformed pending journals fail before any runtime call.
malformed_root="$work/malformed pending root"
malformed_image="$work/malformed-pending.sif"
: > "$malformed_image"
malformed_invoke() {
  "$helper" --apptainer --ct-instance-root "$malformed_root" \
    --ct-bind "$recovery_bind:$work/malformed pending container bind" \
    -- "$malformed_image" /bin/fake-command malformed-pending
}
set +e
CT_INSTANCE_CREATION_NONCE=22222222222222222222222222222222 MKCHAD_TEST_HANG_START=1 \
  CT_INSTANCE_START_TIMEOUT=0.1 malformed_invoke >/dev/null 2>&1
set -e
malformed_pending_files=("$malformed_root"/*.pending)
malformed_pending=${malformed_pending_files[0]:-}
[[ -n $malformed_pending && -e $malformed_pending ]] || {
  printf '%s\n' 'malformed pending fixture did not create a journal' >&2
  exit 1
}
chmod 644 -- "$malformed_pending"
calls_before=$(<"$work/call-count")
set +e
malformed_invoke >/dev/null 2> "$work/malformed-pending.err"
malformed_status=$?
set -e
[[ $malformed_status -eq 1 && $(<"$work/call-count") -eq "$calls_before" \
  && $(<"$work/malformed-pending.err") == *'persistent instance recovery: pending-journal:'* ]] || {
  printf '%s\n' 'malformed pending journal reached the runtime' >&2
  exit 1
}

printf '%s\n' 'container instance executor tests passed'
