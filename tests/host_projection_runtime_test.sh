#!/usr/bin/env bash
# Cumulative, operator-run evidence for host-root projection. This is opt-in:
# fake clients exercise the report contract, but only a returned real-runtime
# report can support a backend claim.
set -euo pipefail

readonly schema='container-tools.host-projection-runtime/v1'
readonly work_root='/tmp/mkchad-v1/host-root-projection-host'
readonly warm_sample_count=20
script_dir=$(dirname "$(realpath "$0")")
tool_root=$(realpath "$script_dir/..")
source_commit=$(git -C "$tool_root" rev-parse HEAD 2>/dev/null || printf '%s' unknown)
architecture=$(uname -m)
caller_group=$(id -g)

usage() {
  printf '%s\n' 'usage: host_projection_runtime_test.sh --backend docker|podman|singularity|apptainer --image LOCAL_IMAGE --work /tmp/mkchad-v1/host-root-projection-host/RUN_ID [--force-fallback]'
  printf '%s\n' '       host_projection_runtime_test.sh --validate-report REPORT.json'
}

json_string() {
  local value=$1
  value=${value//\\/\\\\}
  value=${value//\"/\\\"}
  value=${value//$'\n'/}
  value=${value//$'\r'/}
  printf '"%s"' "${value:0:160}"
}

source_manifest() {
  local file digest
  {
    for file in ct_library.sh ct_exec.sh ct_shell.sh ct_instance_exec.sh tests/bootstrap_test.sh tests/instance_exec_test.sh tests/host_projection_test.sh tests/host_projection_runtime_test.sh; do
      digest=$(sha256sum "$tool_root/$file")
      printf '%s\0%s\0' "$file" "${digest%% *}"
    done
  } | sha256sum | cut -d' ' -f1
}

bounded_version() {
  local value
  value=$($runtime_path --version 2>/dev/null | { IFS= read -r line || true; printf '%s' "${line:-unknown}"; })
  value=${value//[^A-Za-z0-9._+ -]/_}
  printf '%s' "${value:0:120}"
}

filesystem_class() {
  local value
  value=$(stat -f -c %T -- "$work/cache" 2>/dev/null || printf unknown)
  value=${value//[^A-Za-z0-9._-]/_}
  printf '%s' "${value:0:48}"
}

set_case() {
  local id=$1 status=$2 reason=$3 elapsed=$4 operations=$5 timings=$6 cleanup=$7
  case_status[$id]=$status
  case_reason[$id]=$reason
  case_elapsed[$id]=$elapsed
  case_operations[$id]=$operations
  case_timings[$id]=$timings
  case_cleanup[$id]=$cleanup
}

empty_operations='{"inspect":0,"probe_create":0,"probe_start":0,"probe_cleanup":0,"instance_liveness":0,"instance_start":0,"payload":0,"probe_tags":{"direct":0,"fallback":0}}'
empty_timings='{"cold_ms":0,"refresh_ms":0,"samples":0,"median_ms":0,"p95_ms":0,"max_ms":0}'
declare -A case_status=() case_reason=() case_elapsed=() case_operations=() case_timings=() case_cleanup=()

emit_report() {
  local overall=$1 report=${2:-} id separator='' status reason elapsed operations timings cleanup
  {
    printf '{"schema":'; json_string "$schema"
    printf ',"overall":'; json_string "$overall"
    printf ',"backend":'; json_string "$backend"
    printf ',"backend_version":'; json_string "${backend_version:-unavailable}"
    printf ',"architecture":'; json_string "$architecture"
    printf ',"source_commit":'; json_string "$source_commit"
    printf ',"source_manifest":'; json_string "${manifest_current:-unavailable}"
    printf ',"image_id":'; json_string "${image_id:-unavailable}"
    printf ',"image_digest":'; json_string "${image_digest:-unavailable}"
    printf ',"cache_filesystem":'; json_string "${cache_filesystem:-unavailable}"
    printf ',"strategy_condition":'; json_string "${strategy_condition:-unavailable}"
    printf ',"cleanup":'; json_string "${cleanup_overall:-not-needed}"
    printf ',"cases":['
    for id in HP-HOST-001 HP-HOST-002 HP-HOST-003 HP-HOST-004 HP-HOST-005 HP-HOST-006; do
      status=${case_status[$id]:-skip}
      reason=${case_reason[$id]:-unavailable}
      elapsed=${case_elapsed[$id]:-0}
      operations=${case_operations[$id]:-$empty_operations}
      timings=${case_timings[$id]:-$empty_timings}
      cleanup=${case_cleanup[$id]:-not-needed}
      printf '%s{"id":' "$separator"; json_string "$id"
      printf ',"status":'; json_string "$status"
      printf ',"reason":'; json_string "$reason"
      printf ',"elapsed_ms":%s,"operations":%s,"timings":%s,"cleanup":' "$elapsed" "$operations" "$timings"; json_string "$cleanup"
      printf '}'
      separator=,
    done
    printf ']}\n'
  } | if [[ -n $report ]]; then tee "$report"; else cat; fi
}

mark_all() {
  local status=$1 reason=$2
  local id
  for id in HP-HOST-001 HP-HOST-002 HP-HOST-003 HP-HOST-004 HP-HOST-005 HP-HOST-006; do
    if [[ $id == HP-HOST-006 && ( ${backend:-} == docker || ${backend:-} == podman ) ]]; then
      set_case "$id" skip backend-inapplicable 0 "$empty_operations" "$empty_timings" not-needed
    else
      set_case "$id" "$status" "$reason" 0 "$empty_operations" "$empty_timings" not-needed
    fi
  done
}

validate_report() {
  local report=$1 data current_manifest expected_id
  [[ -f $report && $(stat -c %s -- "$report" 2>/dev/null || printf 0) -le 1048576 ]] || return 1
  data=$(<"$report")
  current_manifest=$(source_manifest)
  [[ $data == "{\"schema\":\"$schema\","* ]] || return 1
  [[ $data == *"\"source_commit\":\"$source_commit\""* ]] || return 1
  [[ $data == *"\"source_manifest\":\"$current_manifest\""* ]] || return 1
  [[ $data == *'"overall":"passed"'* || $data == *'"overall":"failed"'* || $data == *'"overall":"unavailable"'* ]] || return 1
  for expected_id in HP-HOST-001 HP-HOST-002 HP-HOST-003 HP-HOST-004 HP-HOST-005 HP-HOST-006; do
    [[ $data == *"\"id\":\"$expected_id\""* ]] || return 1
  done
  # Reports are generated as one compact object; reject likely raw host data.
  [[ $data != *$'\n'* && $data != *'/home/'* && $data != *'/etc/'* && $data != *'"argv"'* && $data != *'"environment"'* ]] || return 1
}

if [[ ${1:-} == --validate-report ]]; then
  [[ $# == 2 ]] || { usage >&2; exit 2; }
  validate_report "$2"
  exit $?
fi

backend=
image=
work=
force_fallback=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --help)
      [[ $# == 1 ]] || { usage >&2; exit 2; }
      usage
      exit 0
      ;;
    --backend)
      [[ $# -ge 2 && -z $backend ]] || { usage >&2; exit 2; }
      backend=$2
      shift 2
      ;;
    --image)
      [[ $# -ge 2 && -z $image ]] || { usage >&2; exit 2; }
      image=$2
      shift 2
      ;;
    --work)
      [[ $# -ge 2 && -z $work ]] || { usage >&2; exit 2; }
      work=$2
      shift 2
      ;;
    --force-fallback)
      [[ $force_fallback == 0 ]] || { usage >&2; exit 2; }
      force_fallback=1
      shift
      ;;
    *) usage >&2; exit 2 ;;
  esac
done

case "$backend" in docker|podman|singularity|apptainer) ;; *) usage >&2; exit 2 ;; esac
[[ -n $image && -n $work && $work == "$work_root"/* && $work != "$work_root"/*/* \
  && ${work##*/} =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ && ! -e $work ]] || { usage >&2; exit 2; }
if [[ $force_fallback == 1 && $backend != docker && $backend != podman ]]; then
  usage >&2
  exit 2
fi
if [[ $backend == singularity || $backend == apptainer ]]; then
  [[ $image == /* && -f $image ]] || { usage >&2; exit 2; }
fi

manifest_current=$(source_manifest)
image_id=unavailable
image_digest=unavailable
cache_filesystem=unavailable
strategy_condition=unavailable
cleanup_overall=not-needed
backend_version=unavailable

# A creation failure is environmental rather than malformed input. It cannot
# write a report file, but stdout records the unavailable gate.
if ! mkdir -p -- "$work_root" || ! mkdir -- "$work"; then
  mark_all skip work-create-failed
  emit_report unavailable
  exit 77
fi
report="$work/report.json"
if [[ ${CT_HOST_PROJECTION_RUNTIME_TEST:-} != 1 ]]; then
  mark_all skip disabled
  emit_report unavailable "$report"
  exit 77
fi

if ! runtime_path=$(command -v -- "$backend"); then
  mark_all skip runtime-unavailable
  emit_report unavailable "$report"
  exit 77
fi
backend_version=$(bounded_version)

case "$backend" in
  docker|podman)
    # `image inspect` only reads local storage. Every launcher call below uses
    # this immutable ID rather than the mutable selector supplied by the caller.
    image_id=$($runtime_path image inspect --format '{{.Id}}' "$image" 2>/dev/null) || {
      mark_all skip local-image-unavailable
      emit_report unavailable "$report"
      exit 77
    }
    [[ $image_id =~ ^sha256:[0-9a-fA-F]{64}$ ]] || {
      mark_all skip local-image-unavailable
      emit_report unavailable "$report"
      exit 77
    }
    image_digest=$image_id
    immutable_image=$image_id
    source_image_digest=$image_id
    ;;
  singularity|apptainer)
    source_image_digest=$(sha256sum -- "$image" | cut -d' ' -f1) || {
      mark_all skip image-unavailable
      emit_report unavailable "$report"
      exit 77
    }
    immutable_image="$work/image.sif"
    cp --reflink=auto -- "$image" "$immutable_image" || {
      mark_all skip image-stage-failed
      emit_report unavailable "$report"
      exit 77
    }
    image_digest=$(sha256sum -- "$immutable_image" | cut -d' ' -f1)
    image_id="sha256:$image_digest"
    ;;
esac

mkdir -p "$work/cache" "$work/results" "$work/home" "$work/fixtures/readable" "$work/fixtures/writable" \
  "$work/fixtures/nonwritable" "$work/fixtures/group-only" "$work/adapter"
chmod 755 "$work/fixtures/readable"
chmod 700 "$work/fixtures/writable"
chmod 500 "$work/fixtures/nonwritable"
chmod 070 "$work/fixtures/group-only"
cache_filesystem=$(filesystem_class)

runtime_sentinel=
if [[ ${XDG_RUNTIME_DIR:-} == /run/* && -d ${XDG_RUNTIME_DIR:-} && -w ${XDG_RUNTIME_DIR:-} ]]; then
  runtime_sentinel="$XDG_RUNTIME_DIR/host-projection-${work##*/}"
  mkdir -- "$runtime_sentinel"
  : > "$runtime_sentinel/sentinel"
fi

# The adapter traces only launcher-under-test calls. Admission and post-run
# metadata calls intentionally use runtime_path directly and never enter this log.
operation_log="$work/operations.log"
adapter="$work/adapter/$backend"
printf '#!/usr/bin/env bash\nset -euo pipefail\nreal_runtime=%q\noperation_log=%q\nforce_fallback=%q\nkind=payload\ntag=none\n' \
  "$runtime_path" "$operation_log" "$force_fallback" > "$adapter"
cat >> "$adapter" <<'EOF'
case "${1:-}" in
  create)
    kind=probe-create
    for argument; do [[ $argument == *source=/,target=/host,* || $argument == *src=/,dst=/host,* ]] && tag=direct; done
    [[ $tag == none ]] && tag=fallback
    if [[ $force_fallback == 1 && $tag == direct ]]; then
      printf '%s:%s\n' "$kind" "$tag" >> "$operation_log"
      exit 12
    fi
    ;;
  start) kind=probe-start ;;
  rm) kind=probe-cleanup ;;
  instance)
    [[ ${2:-} == start ]] && kind=instance-start
    ;;
  exec)
    for argument; do
      [[ $argument == instance://* ]] && { kind=payload; break; }
      [[ $argument == *ct-host-projection-group=native* ]] && { kind=probe-start; tag=fallback; break; }
    done
    if [[ $kind == payload && " $* " == *' instance://'* ]]; then
      for argument; do [[ $argument == -c ]] && { kind=instance-liveness; break; }; done
    fi
    ;;
  run) kind=payload ;;
esac
if [[ $kind == probe-start || $kind == probe-cleanup ]]; then
  [[ -f "${operation_log}.probe-tag" ]] && tag=$(<"${operation_log}.probe-tag")
fi
if [[ $kind == probe-create ]]; then printf '%s' "$tag" > "${operation_log}.probe-tag"; fi
printf '%s:%s\n' "$kind" "$tag" >> "$operation_log"
exec "$real_runtime" "$@"
EOF
chmod 755 "$adapter"

manifest_before_dispatch=$(source_manifest)
if [[ $manifest_before_dispatch != "$manifest_current" ]]; then
  manifest_current=$manifest_before_dispatch
  mark_all fail source-changed
  emit_report failed "$report"
  exit 1
fi

operation_count() {
  local file=$1 needle=$2 count=0 line
  [[ -f $file ]] || { printf 0; return; }
  while IFS= read -r line; do
    if [[ $needle == *:* ]]; then
      [[ $line == "$needle" ]] && count=$((count + 1))
    else
      [[ $line == "$needle":* ]] && count=$((count + 1))
    fi
  done < "$file"
  printf '%s' "$count"
}

operation_json() {
  local file=$1 inspect probe_create probe_start probe_cleanup liveness instance_start payload direct fallback
  inspect=$(operation_count "$file" inspect)
  probe_create=$(operation_count "$file" probe-create)
  probe_start=$(operation_count "$file" probe-start)
  probe_cleanup=$(operation_count "$file" probe-cleanup)
  liveness=$(operation_count "$file" instance-liveness)
  instance_start=$(operation_count "$file" instance-start)
  payload=$(operation_count "$file" payload)
  direct=$(operation_count "$file" probe-create:direct)
  fallback=$(operation_count "$file" probe-create:fallback)
  fallback=$((fallback + $(operation_count "$file" probe-start:fallback)))
  printf '{"inspect":%s,"probe_create":%s,"probe_start":%s,"probe_cleanup":%s,"instance_liveness":%s,"instance_start":%s,"payload":%s,"probe_tags":{"direct":%s,"fallback":%s}}' \
    "$inspect" "$probe_create" "$probe_start" "$probe_cleanup" "$liveness" "$instance_start" "$payload" "$direct" "$fallback"
}

elapsed_ms() {
  local start=$1 end
  end=$(date +%s%N)
  printf '%s' $(((end - start) / 1000000))
}

run_foreground() {
  local phase=$1 mode=$2 refresh=$3 result started payload_script launch_status=0
  local -a refresh_args=()
  result="$work/results/$phase"
  [[ $refresh == 1 ]] && refresh_args=(--ct-host-root-refresh)
  payload_script="set -eu; test -d /usr; test -x /usr/bin/sh; test -d /host; test -d '/host$work'; test -d /host/run; test ! -e /host/proc/self; test -r '/host$work/fixtures/readable'; test -w '/host$work/fixtures/writable'; ! test -w '/host$work/fixtures/nonwritable'; ! test -x '/host$work/fixtures/group-only'; id -G | tr ' ' '\\n' | grep -qx '$caller_group';"
  if [[ -n $runtime_sentinel ]]; then
    payload_script+=" test -f '/host$runtime_sentinel/sentinel';"
  fi
  payload_script+=" printf host-projection-ok > '/host$result'"
  started=$(date +%s%N)
  PATH="$work/adapter:$PATH" HOME="$work/home" CT_MOUNT_CFG="$work/missing-mount-config" \
    CT_HOST_PROJECTION_CACHE_ROOT="$work/cache" CT_HOST_PROJECTION_RUNTIME_LABEL="${work##*/}" \
    "$tool_root/ct_exec.sh" "--$backend" --ct-host-root "$mode" "${refresh_args[@]}" \
    "$immutable_image" /bin/sh -ec "$payload_script" >/dev/null 2>&1 || launch_status=$?
  foreground_elapsed[$phase]=$(elapsed_ms "$started")
  (( launch_status == 0 )) && [[ -f $result ]]
}

declare -A foreground_elapsed=() trace_file=()
overall_status=0
phase=cold
trace_file[$phase]="$work/trace-$phase"
: > "${trace_file[$phase]}"
: > "$operation_log"
if ! run_foreground "$phase" auto 0; then overall_status=1; fi
cp "$operation_log" "${trace_file[$phase]}"

warm_elapsed=()
for ((sample = 1; sample <= warm_sample_count; sample++)); do
  phase="warm-$sample"
  trace_file[$phase]="$work/trace-$phase"
  : > "$operation_log"
  if ! run_foreground "$phase" auto 0; then overall_status=1; fi
  cp "$operation_log" "${trace_file[$phase]}"
  warm_elapsed+=("${foreground_elapsed[$phase]:-0}")
done

trace_file[required]="$work/trace-required"
: > "$operation_log"
required_refused=0
if ! run_foreground required required 0; then required_refused=1; fi
cp "$operation_log" "${trace_file[required]}"

trace_file[refresh]="$work/trace-refresh"
: > "$operation_log"
if ! run_foreground refresh auto 1; then overall_status=1; fi
cp "$operation_log" "${trace_file[refresh]}"

selection_strategy=none
selection_files=("$work/cache"/[0-9a-f][0-9a-f]*)
if [[ -f ${selection_files[0]:-} ]]; then
  mapfile -d '' -t selection_fields < "${selection_files[0]}" || true
  case "${selection_fields[2]:-}" in direct|fallback) selection_strategy=${selection_fields[2]} ;; esac
fi
if [[ $selection_strategy == none ]]; then
  overall_status=1
fi
if [[ $force_fallback == 1 ]]; then
  strategy_condition=forced-fallback
else
  strategy_condition="selected-$selection_strategy"
fi

cold_operations=$(operation_json "${trace_file[cold]}")
required_operations=$(operation_json "${trace_file[required]}")
warm_payloads=0
warm_clean=1
for ((sample = 1; sample <= warm_sample_count; sample++)); do
  current_operations=$(operation_json "${trace_file[warm-$sample]}")
  [[ $current_operations == *'"inspect":0,"probe_create":0,"probe_start":0,"probe_cleanup":0,"instance_liveness":0,"instance_start":0,"payload":1'* ]] || warm_clean=0
  warm_payloads=$((warm_payloads + $(operation_count "${trace_file[warm-$sample]}" payload)))
done

mapfile -t sorted_warm < <(printf '%s\n' "${warm_elapsed[@]}" | sort -n)
warm_median=${sorted_warm[$(((warm_sample_count - 1) / 2))]}
warm_p95=${sorted_warm[$(((warm_sample_count * 95 + 99) / 100 - 1))]}
warm_max=${sorted_warm[$((warm_sample_count - 1))]}
warm_timings=$(printf '{"cold_ms":%s,"refresh_ms":%s,"samples":%s,"median_ms":%s,"p95_ms":%s,"max_ms":%s}' "${foreground_elapsed[cold]}" "${foreground_elapsed[refresh]}" "$warm_sample_count" "$warm_median" "$warm_p95" "$warm_max")

expected_cold=0
case "$backend:$selection_strategy:$force_fallback" in
  docker:direct:0|podman:direct:0)
    [[ $(operation_count "${trace_file[cold]}" probe-create) == 1 && $(operation_count "${trace_file[cold]}" probe-start) == 1 && $(operation_count "${trace_file[cold]}" probe-cleanup) == 1 ]] && expected_cold=1
    ;;
  docker:fallback:*|podman:fallback:*)
    [[ $(operation_count "${trace_file[cold]}" probe-create) == 2 && $(operation_count "${trace_file[cold]}" probe-start) == 1 && $(operation_count "${trace_file[cold]}" probe-cleanup) == 1 ]] && expected_cold=1
    ;;
  singularity:fallback:*|apptainer:fallback:*)
    [[ $(operation_count "${trace_file[cold]}" probe-create) == 0 && $(operation_count "${trace_file[cold]}" probe-start) == 1 && $(operation_count "${trace_file[cold]}" probe-cleanup) == 0 ]] && expected_cold=1
    ;;
esac
[[ $(operation_count "${trace_file[cold]}" payload) == 1 && $(operation_count "${trace_file[refresh]}" payload) == 1 ]] || expected_cold=0
[[ $warm_clean == 1 && $warm_payloads == "$warm_sample_count" ]] || expected_cold=0
if [[ $required_refused == 1 ]]; then
  [[ $required_operations == "$empty_operations" ]] || expected_cold=0
else
  [[ $required_operations == *'"inspect":0,"probe_create":0,"probe_start":0,"probe_cleanup":0,"instance_liveness":0,"instance_start":0,"payload":1'* ]] || expected_cold=0
fi

cleanup_overall=exact
if [[ $backend == docker || $backend == podman ]]; then
  [[ $(operation_count "${trace_file[cold]}" probe-start) == $(operation_count "${trace_file[cold]}" probe-cleanup) ]] || cleanup_overall=failed
  [[ $(operation_count "${trace_file[refresh]}" probe-start) == $(operation_count "${trace_file[refresh]}" probe-cleanup) ]] || cleanup_overall=failed
fi
[[ $cleanup_overall == exact ]] || overall_status=1

if [[ $overall_status == 0 && $expected_cold == 1 ]]; then
  set_case HP-HOST-001 pass selected-strategy "${foreground_elapsed[cold]}" "$cold_operations" "$empty_timings" exact
  set_case HP-HOST-002 pass run-and-kernel-exclusions "${foreground_elapsed[cold]}" "$cold_operations" "$empty_timings" exact
  set_case HP-HOST-003 pass caller-fixtures-and-groups "${foreground_elapsed[cold]}" "$cold_operations" "$empty_timings" exact
  matrix_operations=$(printf '{"cold":%s,"warm_payloads":%s,"refresh":%s,"required":%s}' "$cold_operations" "$warm_payloads" "$(operation_json "${trace_file[refresh]}")" "$required_operations")
  set_case HP-HOST-004 pass cold-warm-refresh "$(( foreground_elapsed[cold] + foreground_elapsed[refresh] ))" "$matrix_operations" "$warm_timings" exact
  if [[ $required_refused == 1 ]]; then
    set_case HP-HOST-005 pass required-refused-before-payload "${foreground_elapsed[required]}" "$required_operations" "$empty_timings" exact
  else
    set_case HP-HOST-005 pass auto-required-exact-once "${foreground_elapsed[required]}" "$required_operations" "$empty_timings" exact
  fi
else
  overall_status=1
  set_case HP-HOST-001 fail matrix-incomplete 0 "$cold_operations" "$empty_timings" "$cleanup_overall"
  set_case HP-HOST-002 fail matrix-incomplete 0 "$cold_operations" "$empty_timings" "$cleanup_overall"
  set_case HP-HOST-003 fail matrix-incomplete 0 "$cold_operations" "$empty_timings" "$cleanup_overall"
  set_case HP-HOST-004 fail matrix-incomplete 0 "$cold_operations" "$warm_timings" "$cleanup_overall"
  set_case HP-HOST-005 fail matrix-incomplete 0 "$required_operations" "$empty_timings" "$cleanup_overall"
fi

case "$backend" in
  docker|podman)
    set_case HP-HOST-006 skip backend-inapplicable 0 "$empty_operations" "$empty_timings" not-needed
    ;;
  singularity|apptainer)
    persistent_trace="$work/trace-persistent-reuse"
    : > "$operation_log"
    persistent_started=$(date +%s%N)
    persistent_ok=1
    for phase in persistent-first persistent-reuse; do
      persistent_result="$work/results/$phase"
      PATH="$work/adapter:$PATH" HOME="$work/home" CT_MOUNT_CFG="$work/missing-mount-config" \
        CT_HOST_PROJECTION_CACHE_ROOT="$work/cache" \
        "$tool_root/ct_instance_exec.sh" "--$backend" --ct-instance-root "$work/persistent-instances" \
        --ct-host-root auto "$immutable_image" /bin/sh -ec "printf host-projection-ok > '/host$persistent_result'" >/dev/null 2>&1 || persistent_ok=0
      [[ -f $persistent_result ]] || persistent_ok=0
    done
    cp "$operation_log" "$persistent_trace"
    persistent_elapsed=$(elapsed_ms "$persistent_started")
    persistent_operations=$(operation_json "$persistent_trace")
    [[ $persistent_operations == *'"instance_liveness":'* ]] || persistent_ok=0
    # The final reuse call is the last two adapter events before cleanup.
    mapfile -t persistent_events < "$persistent_trace"
    persistent_count=${#persistent_events[@]}
    (( persistent_count >= 2 )) && [[ ${persistent_events[persistent_count - 2]} == instance-liveness:* && ${persistent_events[persistent_count - 1]} == payload:* ]] || persistent_ok=0
    persistent_name=
    identity_files=("$work/persistent-instances"/*.identity)
    if [[ -f ${identity_files[0]:-} ]]; then
      while IFS= read -r line; do [[ $line == name=mkchad-* ]] && persistent_name=${line#name=}; done < "${identity_files[0]}"
    fi
    if [[ $persistent_name =~ ^mkchad-[0-9a-f][0-9a-f][0-9a-f][0-9a-f] ]]; then
      "$runtime_path" instance stop "$persistent_name" >/dev/null 2>&1 || persistent_ok=0
    else
      persistent_ok=0
    fi
    if [[ $persistent_ok == 1 ]]; then
      set_case HP-HOST-006 pass persistent-profile-reuse "$persistent_elapsed" "$persistent_operations" "$empty_timings" exact
    else
      set_case HP-HOST-006 fail persistent-check-failed "$persistent_elapsed" "$persistent_operations" "$empty_timings" failed
      overall_status=1
      cleanup_overall=failed
    fi
    ;;
esac

manifest_after=$(source_manifest)
integrity_reason=
[[ $manifest_after == "$manifest_before_dispatch" ]] || integrity_reason=source-changed
case "$backend" in
  docker|podman)
    current_image=$($runtime_path image inspect --format '{{.Id}}' "$image" 2>/dev/null || true)
    [[ $current_image == "$image_id" ]] || integrity_reason='image-changed'
    ;;
  singularity|apptainer)
    [[ $(sha256sum -- "$image" | cut -d' ' -f1) == "$source_image_digest" ]] || integrity_reason='image-changed'
    [[ $(sha256sum -- "$immutable_image" | cut -d' ' -f1) == "$image_digest" ]] || integrity_reason='image-changed'
    ;;
esac
manifest_current=$manifest_after
if [[ -n $integrity_reason ]]; then
  overall_status=1
  mark_all fail "$integrity_reason"
fi
if [[ -n $runtime_sentinel ]]; then
  if ! rm -f -- "$runtime_sentinel/sentinel" || ! rmdir -- "$runtime_sentinel"; then
    cleanup_overall=failed
    overall_status=1
    mark_all fail cleanup-failed
  fi
fi

if [[ $overall_status == 0 ]]; then
  emit_report passed "$report"
  exit 0
fi
emit_report failed "$report"
exit 1
