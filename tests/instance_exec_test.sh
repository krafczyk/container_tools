#!/usr/bin/env bash
set -euo pipefail

work=${1:?pass a task-specific directory beneath /tmp/opencode-mkchad}
helper=${2:?pass the ct_instance_exec.sh path}
helper=$(realpath "$helper")
work=$(realpath -m -- "$work")
[[ $work == /tmp/opencode-mkchad/* ]] || { printf '%s\n' 'test directory must be beneath /tmp/opencode-mkchad' >&2; exit 2; }
[[ ! -e $work ]] || { printf '%s\n' 'test directory already exists' >&2; exit 2; }

fake="$work/fake-bin"
home="$work/home"
calls="$work/calls"
instances="$work/instances"
instance_root="$work/instance root"
image_path="$work/image.sif"
bootstrap="$work/bootstrap"
bind_source="$work/bind source"
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
  [[ ${MKCHAD_TEST_HANG_START:-} != 1 ]] || sleep 10
  sleep "${MKCHAD_TEST_START_DELAY:-0}"
  [[ ! -e $MKCHAD_TEST_INSTANCES/$name ]] || exit 42
  : > "$MKCHAD_TEST_INSTANCES/$name"
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
  if [[ $command == /bin/true ]]; then
    [[ ${MKCHAD_TEST_HANG_PROBE:-} != 1 ]] || sleep 10
    if [[ -n ${MKCHAD_TEST_MUTATE_ON_PROBE:-} && ! -e ${MKCHAD_TEST_MUTATE_MARKER:-} ]]; then
      printf 'mutated during probe\n' >> "$MKCHAD_TEST_MUTATE_ON_PROBE"
      : > "$MKCHAD_TEST_MUTATE_MARKER"
    fi
    exit 0
  fi
  exit "${MKCHAD_TEST_PAYLOAD_STATUS:-23}"
fi

exit 64
EOF
chmod 755 "$fake/apptainer"

export HOME="$home"
export PATH="$fake:$PATH"
export CT_MOUNT_CFG="$work/missing-mount-config"
export MKCHAD_TEST_CALLS="$calls"
export MKCHAD_TEST_CALL_COUNT="$work/call-count"
export MKCHAD_TEST_CALL_LOCK="$work/call.lock"
export MKCHAD_TEST_INSTANCES="$instances"

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
if contains_line "$calls/4" --bind; then
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

printf '%s\n' 'container instance executor tests passed'
