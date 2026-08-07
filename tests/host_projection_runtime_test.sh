#!/usr/bin/env bash
# Cumulative, operator-run evidence for host-root projection. This is opt-in:
# fake clients exercise the report contract, but only a returned real-runtime
# report can support a backend claim.
set -euo pipefail

readonly schema='container-tools.host-projection-runtime/v1'
readonly work_root='/tmp/mkchad-v1/host-root-projection-host'
readonly warm_sample_count=20
readonly operation_capture_fd=190
readonly probe_tag_capture_fd=191
readonly identity_capture_fd=192
readonly runtime_executable_fd=193
readonly case_ids=(HP-HOST-001 HP-HOST-002 HP-HOST-003 HP-HOST-004 HP-HOST-005 HP-HOST-006)
report_emitted=0
script_dir=$(dirname "$(realpath "$0")")
tool_root=$(realpath "$script_dir/..")
source_commit=$(git -C "$tool_root" rev-parse HEAD 2>/dev/null || printf '%s' unknown)
architecture=$(uname -m)
caller_group=$(id -g)

usage() {
  printf '%s\n' 'usage: host_projection_runtime_test.sh --backend docker|podman|singularity|apptainer --image LOCAL_IMAGE --work /tmp/mkchad-v1/host-root-projection-host/RUN_ID [--force-fallback]'
  printf '%s\n' '       host_projection_runtime_test.sh --validate-report REPORT.json'
  printf '%s\n' '       host_projection_runtime_test.sh --validate-fixture-report REPORT.json'
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
  local file file_digest manifest_digest
  manifest_digest=$({
    for file in ct_args.sh ct_mount_detector.sh ct_library.sh ct_exec.sh ct_shell.sh ct_instance_exec.sh tests/bootstrap_test.sh tests/instance_exec_test.sh tests/host_projection_test.sh tests/host_projection_runtime_test.sh; do
      file_digest=$(sha256sum "$tool_root/$file")
      printf '%s\0%s\0' "$file" "${file_digest%% *}"
    done
  } | sha256sum)
  printf '%s' "${manifest_digest%% *}"
}

bounded_version() {
  local value
  value=$(timeout --foreground --kill-after=1s 3s "$runtime_path" --version 2>/dev/null | { IFS= read -r line || true; printf '%s' "${line:-unknown}"; })
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
  local phase_operations=${8:-}
  [[ -n $phase_operations ]] || phase_operations='{}'
  case_status[$id]=$status
  case_reason[$id]=$reason
  case_elapsed[$id]=$elapsed
  case_operations[$id]=$operations
  case_timings[$id]=$timings
  case_cleanup[$id]=$cleanup
  case_phase_operations[$id]=$phase_operations
}

empty_operations='{"inspect":0,"probe_create":0,"probe_start":0,"probe_cleanup":0,"instance_liveness":0,"instance_start":0,"instance_stop":0,"payload":0,"probe_tags":{"direct":0,"fallback":0}}'
empty_timings='{"cold_ms":0,"refresh_ms":0,"samples":0,"median_ms":0,"p95_ms":0,"max_ms":0}'
declare -A case_status=() case_reason=() case_elapsed=() case_operations=() case_timings=() case_cleanup=() case_phase_operations=()

emit_report() {
  local overall=$1 report=${2:-} id separator='' status reason elapsed operations timings cleanup phase_operations
  {
    printf '{"schema":'; json_string "$schema"
    printf ',"evidence_kind":'; json_string "${evidence_kind:-host}"
    printf ',"overall":'; json_string "$overall"
    printf ',"backend":'; json_string "$backend"
    printf ',"backend_version":'; json_string "${backend_version:-unavailable}"
    printf ',"backend_executable_digest":'; json_string "${backend_executable_digest:-unavailable}"
    printf ',"architecture":'; json_string "$architecture"
    printf ',"source_commit":'; json_string "$source_commit"
    printf ',"source_manifest":'; json_string "${manifest_current:-unavailable}"
    printf ',"image_id":'; json_string "${image_id:-unavailable}"
    printf ',"image_digest":'; json_string "${image_digest:-unavailable}"
    printf ',"cache_filesystem":'; json_string "${cache_filesystem:-unavailable}"
    printf ',"strategy_condition":'; json_string "${strategy_condition:-unavailable}"
    printf ',"cleanup":'; json_string "${cleanup_overall:-not-needed}"
    printf ',"cases":['
    for id in "${case_ids[@]}"; do
      status=${case_status[$id]:-skip}
      reason=${case_reason[$id]:-unavailable}
      elapsed=${case_elapsed[$id]:-0}
      operations=${case_operations[$id]:-$empty_operations}
      timings=${case_timings[$id]:-$empty_timings}
      cleanup=${case_cleanup[$id]:-not-needed}
      phase_operations=${case_phase_operations[$id]:-'{}'}
      printf '%s{"id":' "$separator"; json_string "$id"
      printf ',"status":'; json_string "$status"
      printf ',"reason":'; json_string "$reason"
      printf ',"elapsed_ms":%s,"operations":%s,"phase_operations":%s,"timings":%s,"cleanup":' "$elapsed" "$operations" "$phase_operations" "$timings"; json_string "$cleanup"
      printf '}'
      separator=,
    done
    printf ']}\n'
  } | if [[ -n $report ]]; then tee "$report"; else cat; fi
  report_emitted=1
}

mark_all() {
  local status=$1 reason=$2
  local id
  for id in "${case_ids[@]}"; do
    if [[ $id == HP-HOST-006 && ( ${backend:-} == docker || ${backend:-} == podman ) ]]; then
      set_case "$id" skip backend-inapplicable 0 "$empty_operations" "$empty_timings" not-needed
    else
      set_case "$id" "$status" "$reason" 0 "$empty_operations" "$empty_timings" not-needed
    fi
  done
}

report_structure_valid() {
  local report=$1 expected_kind=$2
  [[ -f $report && $(stat -c %s -- "$report" 2>/dev/null || printf 0) -le 1048576 ]] || return 1
  jq -s -e --arg schema "$schema" --arg kind "$expected_kind" '
    def operations:
      type == "object" and (keys | sort) == ["inspect", "instance_liveness", "instance_start", "instance_stop", "payload", "probe_cleanup", "probe_create", "probe_start", "probe_tags"] and
      ([.inspect, .probe_create, .probe_start, .probe_cleanup, .instance_liveness, .instance_start, .instance_stop, .payload] | all(.[]; type == "number" and floor == . and . >= 0)) and
      (.probe_tags | type == "object" and (keys | sort) == ["direct", "fallback"] and ([.direct, .fallback] | all(.[]; type == "number" and floor == . and . >= 0)));
    def timings:
      type == "object" and (keys | sort) == ["cold_ms", "max_ms", "median_ms", "p95_ms", "refresh_ms", "samples"] and
      ([.cold_ms, .refresh_ms, .samples, .median_ms, .p95_ms, .max_ms] | all(.[]; type == "number" and floor == . and . >= 0));
    def case_valid:
      type == "object" and (keys | sort) == ["cleanup", "elapsed_ms", "id", "operations", "phase_operations", "reason", "status", "timings"] and
      (.id | type == "string" and test("^HP-HOST-00[1-6]$")) and
      (.status | type == "string" and IN("pass", "fail", "skip")) and
      (.reason | type == "string" and test("^[A-Za-z0-9._-]{1,160}$")) and
      (.elapsed_ms | type == "number" and floor == . and . >= 0) and
      (.cleanup | type == "string" and IN("exact", "failed", "not-needed")) and
      (.operations | operations) and
      (if .id == "HP-HOST-004" and .status != "skip" then
        (.phase_operations | type == "object" and (keys | sort) == ["cold", "refresh", "required", "warm"] and all(.[]; operations))
      else
        .phase_operations == {}
      end) and
      (.timings | timings);
    length == 1 and (.[0] |
    type == "object" and
    (keys | sort) == ["architecture", "backend", "backend_executable_digest", "backend_version", "cache_filesystem", "cases", "cleanup", "evidence_kind", "image_digest", "image_id", "overall", "schema", "source_commit", "source_manifest", "strategy_condition"] and
    .schema == $schema and .evidence_kind == $kind and
    (.overall | type == "string" and IN("passed", "failed", "unavailable")) and
    ([.backend, .backend_version, .backend_executable_digest, .architecture, .source_commit, .source_manifest, .image_id, .image_digest, .cache_filesystem, .strategy_condition, .cleanup] | all(.[]; type == "string" and length <= 160)) and
    (.backend_executable_digest | test("^[0-9a-f]{64}$|^unavailable$")) and
    (.backend | IN("docker", "podman", "singularity", "apptainer")) and
    (.cleanup | IN("exact", "failed", "not-needed")) and
    (.cases | type == "array" and length == 6 and all(.[]; case_valid) and ([.[].id] | sort) == ["HP-HOST-001", "HP-HOST-002", "HP-HOST-003", "HP-HOST-004", "HP-HOST-005", "HP-HOST-006"])
    )
  ' "$report" >/dev/null 2>&1
}

validate_report() {
  local report=$1 expected_kind=$2 current_manifest overall backend
  report_structure_valid "$report" "$expected_kind" || return 2
  overall=$(jq -r '.overall' "$report")
  [[ $overall == unavailable ]] && return 77
  [[ $overall == passed ]] || return 1
  current_manifest=$(source_manifest)
  backend=$(jq -r '.backend' "$report")
  jq -e --arg commit "$source_commit" --arg manifest "$current_manifest" '
    def ops($inspect; $create; $start; $cleanup; $liveness; $instance_start; $instance_stop; $payload; $direct; $fallback):
      {inspect:$inspect, probe_create:$create, probe_start:$start, probe_cleanup:$cleanup,
       instance_liveness:$liveness, instance_start:$instance_start, instance_stop:$instance_stop,
       payload:$payload, probe_tags:{direct:$direct, fallback:$fallback}};
    def case_by_id($id): .cases[] | select(.id == $id);
    (case_by_id("HP-HOST-004").phase_operations) as $phases |
    .source_commit == $commit and .source_manifest == $manifest and
    (.source_commit | test("^[0-9a-f]{40,64}$")) and (.source_manifest | test("^[0-9a-f]{64}$")) and
    (.backend_executable_digest | test("^[0-9a-f]{64}$")) and
    .cleanup == "exact" and
    ($phases.cold == $phases.refresh) and
    ($phases.warm == ops(0;0;0;0;0;0;0;20;0;0)) and
    (($phases.required == ops(0;0;0;0;0;0;0;0;0;0)) or
     ($phases.required == ops(0;0;0;0;0;0;0;1;0;0))) and
    (case_by_id("HP-HOST-004").timings.samples == 20) and
    (case_by_id("HP-HOST-001").operations == $phases.cold) and
    (case_by_id("HP-HOST-002").operations == $phases.cold) and
    (case_by_id("HP-HOST-003").operations == $phases.cold) and
    (case_by_id("HP-HOST-004").operations == $phases.cold) and
    (case_by_id("HP-HOST-005").operations == $phases.required) and
    (case_by_id("HP-HOST-001").reason == "selected-strategy") and
    (case_by_id("HP-HOST-002").reason == "run-and-kernel-exclusions") and
    (case_by_id("HP-HOST-003").reason == "caller-fixtures-and-groups") and
    (case_by_id("HP-HOST-004").reason == "cold-warm-refresh") and
    (case_by_id("HP-HOST-005").reason | IN("auto-required-exact-once", "required-refused-before-payload")) and
    (if .backend == "docker" or .backend == "podman" then
      (.image_id == .image_digest and (.image_id | test("^sha256:[0-9a-fA-F]{64}$"))) and
      (.strategy_condition | test("^(selected-(direct|fallback)|forced-fallback)$")) and
      (if .strategy_condition == "selected-direct" then
        $phases.cold == ops(0;1;1;1;0;0;0;1;1;0)
      else
        $phases.cold == ops(0;2;1;1;0;0;0;1;1;2)
      end) and
      (case_by_id("HP-HOST-006") | .status == "skip" and .reason == "backend-inapplicable" and .cleanup == "not-needed")
    else
      (.image_id == ("sha256:" + .image_digest) and (.image_digest | test("^[0-9a-f]{64}$"))) and
      (.strategy_condition == "selected-fallback") and
      ($phases.cold == ops(0;0;1;0;0;0;0;1;0;1)) and
      (case_by_id("HP-HOST-006") | .status == "pass" and .reason == "persistent-profile-reuse" and .cleanup == "exact" and
        .operations == ops(0;0;0;0;1;0;0;1;0;0))
    end) and
    ([.cases[] | select(.id != "HP-HOST-006" or (.status != "skip")) | .status == "pass" and .cleanup == "exact"] | all)
  ' "$report" >/dev/null 2>&1
}

if [[ ${1:-} == --validate-report ]]; then
  [[ $# == 2 ]] || { usage >&2; exit 2; }
  command -v jq >/dev/null 2>&1 || { printf '%s\n' 'host report validation requires jq' >&2; exit 77; }
  validate_report "$2" host
  status=$?
  (( status == 2 )) && exit 2
  exit "$status"
fi

if [[ ${1:-} == --validate-fixture-report ]]; then
  [[ $# == 2 ]] || { usage >&2; exit 2; }
  command -v jq >/dev/null 2>&1 || { printf '%s\n' 'host report validation requires jq' >&2; exit 77; }
  validate_report "$2" fixture
  status=$?
  (( status == 2 )) && exit 2
  exit "$status"
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
evidence_kind=${CT_HOST_PROJECTION_RUNTIME_FIXTURE:+fixture}
evidence_kind=${evidence_kind:-host}
image_id=unavailable
image_digest=unavailable
cache_filesystem=unavailable
strategy_condition=unavailable
cleanup_overall=not-needed
backend_version=unavailable
backend_executable_digest=unavailable

setup_failure() {
  local reason=$1
  mark_all skip "$reason"
  cleanup_overall=failed
  emit_report unavailable "${report:-}"
  exit 77
}

# A creation failure is environmental rather than malformed input. It cannot
# write a report file, but stdout records the unavailable gate.
if ! mkdir -p -- "$work_root" || ! mkdir -- "$work"; then
  mark_all skip work-create-failed
  emit_report unavailable
  exit 77
fi
report="$work/report.json"
runner_finished=0

cleanup_runtime_sentinel() {
  [[ -n ${runtime_sentinel:-} ]] || return 0
  rm -f -- "$runtime_sentinel/sentinel" >/dev/null 2>&1 || return 1
  rmdir -- "$runtime_sentinel" >/dev/null 2>&1 || return 1
  runtime_sentinel=
}

# Valid invocations emit failed evidence even when setup exits unexpectedly.
# shellcheck disable=SC2329 # Signal and EXIT traps invoke this handler.
handle_setup_exit() {
  local status=$? reason=${1:-unexpected-exit}
  (( runner_finished == 0 && report_emitted == 0 )) || return "$status"
  trap - HUP INT TERM EXIT
  cleanup_runtime_sentinel || cleanup_overall=failed
  mark_all fail "$reason"
  emit_report failed "$report"
  (( status != 0 )) || status=1
  exit "$status"
}
trap 'handle_setup_exit interrupted' HUP INT TERM
trap 'handle_setup_exit unexpected-exit' EXIT

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
runtime_path=$(realpath -- "$runtime_path") || setup_failure runtime-unavailable
if [[ $evidence_kind == host && $runtime_path == /tmp/mkchad-v1/host-root-projection/* ]]; then
  setup_failure fixture-runtime-unlabeled
fi
backend_executable_digest=$(sha256sum -- "$runtime_path" | cut -d' ' -f1) || setup_failure runtime-unavailable
backend_version=$(bounded_version)

case "$backend" in
  docker|podman)
    # `image inspect` only reads local storage. Every launcher call below uses
    # this immutable ID rather than the mutable selector supplied by the caller.
    image_id=$(timeout --foreground --kill-after=1s 5s "$runtime_path" image inspect --format '{{.Id}}' "$image" 2>/dev/null) || {
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

if ! mkdir -p "$work/cache" "$work/results" "$work/home" "$work/native-instances" "$work/fixtures/readable" "$work/fixtures/writable" \
  "$work/fixtures/nonwritable" "$work/fixtures/group-only" "$work/adapter" \
  || ! chmod 755 "$work/fixtures/readable" \
  || ! chmod 700 "$work/fixtures/writable" \
  || ! chmod 500 "$work/fixtures/nonwritable" \
  || ! chmod 070 "$work/fixtures/group-only"; then
  setup_failure work-setup-failed
fi
cache_filesystem=$(filesystem_class)

runtime_sentinel=
if [[ ${XDG_RUNTIME_DIR:-} == /run/* && -d ${XDG_RUNTIME_DIR:-} && -w ${XDG_RUNTIME_DIR:-} ]]; then
  runtime_sentinel="$XDG_RUNTIME_DIR/host-projection-${work##*/}"
  if ! mkdir -- "$runtime_sentinel" || ! : > "$runtime_sentinel/sentinel"; then
    setup_failure runtime-sentinel-failed
  fi
fi
if [[ $evidence_kind == fixture && ${CT_HOST_PROJECTION_RUNTIME_ABORT_DURING_SETUP:-} == 1 ]]; then
  false
fi

# The adapter traces only launcher-under-test calls. Admission and post-run
# metadata calls intentionally use runtime_path directly and never enter this channel.
adapter="$work/adapter/$backend"
printf '#!/usr/bin/env bash\nset -euo pipefail\nreal_runtime=%q\nforce_fallback=%q\nkind=payload\ntag=none\ncapture_fd=%q\noperation_fd=%q\nprobe_tag_fd=%q\nruntime_executable_fd=%q\n' \
  "$runtime_path" "$force_fallback" "$identity_capture_fd" "$operation_capture_fd" "$probe_tag_capture_fd" "$runtime_executable_fd" > "$adapter"
cat >> "$adapter" <<'EOF'
[[ $operation_fd =~ ^[0-9]+$ && -e /proc/self/fd/$operation_fd ]] || exit 125
[[ $probe_tag_fd =~ ^[0-9]+$ && -e /proc/self/fd/$probe_tag_fd ]] || exit 125
record_operation() {
  printf '%s:%s\n' "$kind" "$tag" >&"$operation_fd"
}
close_capture_fds() {
  exec {operation_fd}>&-
  exec {probe_tag_fd}>&-
  if [[ -e /proc/self/fd/$capture_fd ]]; then
    exec {capture_fd}>&-
  fi
  if [[ -e /proc/self/fd/$runtime_executable_fd ]]; then
    exec {runtime_executable_fd}<&-
  fi
}
case "${1:-}" in
  create)
    kind=probe-create
    for argument; do [[ $argument == *source=/,target=/host,* || $argument == *src=/,dst=/host,* ]] && tag=direct; done
    [[ $tag == none ]] && tag=fallback
    if [[ $force_fallback == 1 && $tag == direct ]]; then
      record_operation
      close_capture_fds
      exit 20
    fi
    ;;
  start) kind=probe-start ;;
  rm) kind=probe-cleanup ;;
  container)
    [[ ${2:-} == inspect ]] && kind=inspect
    ;;
  instance)
    if [[ ${2:-} == start ]]; then
      kind=instance-start
      if [[ -e /proc/self/fd/$capture_fd ]]; then
        args=("$@")
        name=${!#}
        identity_source=
        for ((index = 0; index < ${#args[@]} - 1; index++)); do
          if [[ ${args[index]} == --bind && ${args[index + 1]} == *:/.container-tools-instance-identity:ro ]]; then
            identity_source=${args[index + 1]%:/.container-tools-instance-identity:ro}
            break
          fi
        done
        mapfile -t identity_fields 2>/dev/null < "$identity_source" || identity_fields=()
        if [[ ${#identity_fields[@]} == 3 && $name =~ ^mkchad-[0-9a-f]{32}$ \
          && ${identity_fields[0]} == "$name" && ${identity_fields[1]} =~ ^[0-9a-f]{64}$ \
          && ${identity_fields[2]} =~ ^[0-9a-f]{32}$ ]]; then
          printf '%s %s %s\n' "$name" "${identity_fields[1]}" "${identity_fields[2]}" >&"$capture_fd"
        fi
      fi
    elif [[ ${2:-} == stop ]]; then
      kind=instance-stop
    elif [[ ${2:-} == list ]]; then
      kind=inspect
    fi
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
if [[ ( $kind == probe-start || $kind == probe-cleanup ) && $tag == none ]]; then
  IFS= read -r tag < "/proc/self/fd/$probe_tag_fd" || tag=none
fi
if [[ $kind == probe-create ]]; then
  : > "/proc/self/fd/$probe_tag_fd"
  printf '%s\n' "$tag" > "/proc/self/fd/$probe_tag_fd"
fi
record_operation
close_capture_fds
exec "$real_runtime" "$@"
EOF
chmod 755 "$adapter" || setup_failure adapter-setup-failed
exec {adapter_read}< "$adapter"
rm -f -- "$adapter" || setup_failure adapter-setup-failed

# Probe strategy state has no pathname while any runtime process can execute.
probe_tag_path=$(mktemp "$work/.probe-tag.XXXXXX") || setup_failure adapter-setup-failed
exec {probe_tag_fd}<> "$probe_tag_path"
rm -f -- "$probe_tag_path" || setup_failure adapter-setup-failed

# The record has no pathname by the time a container can run. The adapter writes
# only the exact start identity to this descriptor and closes it before exec.
capture_path=$(mktemp "$work/.instance-capture.XXXXXX") || setup_failure capture-setup-failed
exec {identity_capture_read}< "$capture_path"
exec {identity_capture_write}> "$capture_path"
rm -f -- "$capture_path" || setup_failure capture-setup-failed
identity_capture_write_open=1
adapter_digest=$(sha256sum -- "/proc/self/fd/$adapter_read" | cut -d' ' -f1)
owned_instance_name=
owned_instance_profile=
owned_instance_nonce=
owned_instance_loaded=0

verify_dispatch_integrity() {
  local current_manifest current_adapter_digest current_image_digest current_backend_digest
  dispatch_integrity_reason=
  current_manifest=$(source_manifest) || return 1
  current_adapter_digest=$(sha256sum -- "/proc/self/fd/$adapter_read" | cut -d' ' -f1) || return 1
  current_backend_digest=$(sha256sum -- "$runtime_path" | cut -d' ' -f1) || return 1
  [[ $current_manifest == "$manifest_current" ]] || { dispatch_integrity_reason='source-changed'; return 1; }
  [[ $current_adapter_digest == "$adapter_digest" ]] || { dispatch_integrity_reason='adapter-changed'; return 1; }
  [[ $current_backend_digest == "$backend_executable_digest" ]] || { dispatch_integrity_reason='runtime-changed'; return 1; }
  case "$backend" in
    docker|podman)
      [[ $immutable_image == "$image_id" && $image_id =~ ^sha256:[0-9a-fA-F]{64}$ ]] || {
        dispatch_integrity_reason='image-changed'
        return 1
      }
      ;;
    singularity|apptainer)
      current_image_digest=$(sha256sum -- "$immutable_image" | cut -d' ' -f1) || return 1
      [[ $current_image_digest == "$image_digest" ]] || { dispatch_integrity_reason='image-changed'; return 1; }
      ;;
  esac
}

load_owned_instance() {
  local -a records=()
  (( owned_instance_loaded == 0 )) || return 0
  if (( identity_capture_write_open )); then
    exec {identity_capture_write}>&-
    identity_capture_write_open=0
  fi
  mapfile -t records <&"$identity_capture_read" || return 1
  owned_instance_loaded=1
  (( ${#records[@]} == 1 )) || return 2
  read -r owned_instance_name owned_instance_profile owned_instance_nonce <<< "${records[0]}"
  [[ $owned_instance_name =~ ^mkchad-[0-9a-f]{32}$ \
    && $owned_instance_profile =~ ^[0-9a-f]{64}$ \
    && $owned_instance_nonce =~ ^[0-9a-f]{32}$ ]] || return 1
}

cleanup_owned_instance() {
  local -a fixture_environment=()
  [[ -n ${owned_instance_name:-} ]] || return 0
  verify_dispatch_integrity || return 1
  [[ $evidence_kind != fixture ]] || fixture_environment=(env "MKCHAD_TEST_RUNTIME_INSTANCES=$work/native-instances")
  # shellcheck disable=SC2016 # The instance shell reads its pinned identity record.
  "${fixture_environment[@]}" timeout --foreground --kill-after=1s 2s "$runtime_path" exec "instance://$owned_instance_name" /bin/sh -c '
    exec 3</.container-tools-instance-identity || exit 42
    IFS= read -r actual_name <&3 || exit 42
    IFS= read -r actual_profile <&3 || exit 42
    IFS= read -r actual_nonce <&3 || exit 42
    [ "$actual_name" = "$1" ] && [ "$actual_profile" = "$2" ] && [ "$actual_nonce" = "$3" ]
  ' sh "$owned_instance_name" "$owned_instance_profile" "$owned_instance_nonce" >/dev/null 2>&1 || return 1
  "${fixture_environment[@]}" timeout --foreground --kill-after=1s 2s "$runtime_path" instance stop "$owned_instance_name" >/dev/null 2>&1
}

manifest_before_dispatch=$(source_manifest)
if [[ $manifest_before_dispatch != "$manifest_current" ]]; then
  manifest_current=$manifest_before_dispatch
  mark_all fail source-changed
  emit_report failed "$report"
  exit 1
fi

# shellcheck disable=SC2329 # The signal trap invokes this cleanup handler.
handle_interruption() {
  local identity_status
  trap - HUP INT TERM EXIT
  overall_status=1
  if load_owned_instance; then
    cleanup_owned_instance || cleanup_overall=failed
  else
    identity_status=$?
    (( identity_status == 2 )) || cleanup_overall=failed
  fi
  cleanup_runtime_sentinel || cleanup_overall=failed
  mark_all fail interrupted
  emit_report failed "$report"
  exit 1
}

# Install bounded exact-owned cleanup before a launcher can create runtime state.
trap - HUP INT TERM EXIT
trap handle_interruption HUP INT TERM

# Any unexpected shell failure after dispatch authority is established still
# emits one failed report and attempts only descriptor-captured instance cleanup.
# shellcheck disable=SC2329 # The EXIT trap invokes this cleanup handler.
handle_unexpected_exit() {
  local status=$? identity_status
  (( runner_finished == 0 )) || return "$status"
  trap - HUP INT TERM EXIT
  if load_owned_instance; then
    cleanup_owned_instance || cleanup_overall=failed
  else
    identity_status=$?
    (( identity_status == 2 )) || cleanup_overall=failed
  fi
  cleanup_runtime_sentinel || cleanup_overall=failed
  mark_all fail unexpected-exit
  emit_report failed "$report"
  (( status != 0 )) || status=1
  exit "$status"
}
trap handle_unexpected_exit EXIT

operation_count() {
  local data=$1 needle=$2 count=0 line
  while IFS= read -r line; do
    if [[ $needle == *:* ]]; then
      [[ $line == "$needle" ]] && count=$((count + 1))
    else
      [[ $line == "$needle":* ]] && count=$((count + 1))
    fi
  done <<< "$data"
  printf '%s' "$count"
}

operation_json() {
  local data=$1 inspect probe_create probe_start probe_cleanup liveness instance_start instance_stop payload direct fallback
  inspect=$(operation_count "$data" inspect)
  probe_create=$(operation_count "$data" probe-create)
  probe_start=$(operation_count "$data" probe-start)
  probe_cleanup=$(operation_count "$data" probe-cleanup)
  liveness=$(operation_count "$data" instance-liveness)
  instance_start=$(operation_count "$data" instance-start)
  instance_stop=$(operation_count "$data" instance-stop)
  payload=$(operation_count "$data" payload)
  direct=$(operation_count "$data" probe-create:direct)
  fallback=$(operation_count "$data" probe-create:fallback)
  fallback=$((fallback + $(operation_count "$data" probe-start:fallback)))
  printf '{"inspect":%s,"probe_create":%s,"probe_start":%s,"probe_cleanup":%s,"instance_liveness":%s,"instance_start":%s,"instance_stop":%s,"payload":%s,"probe_tags":{"direct":%s,"fallback":%s}}' \
    "$inspect" "$probe_create" "$probe_start" "$probe_cleanup" "$liveness" "$instance_start" "$instance_stop" "$payload" "$direct" "$fallback"
}

declare -A trace_data=()
operation_capture_start() {
  local capture_path
  capture_path=$(mktemp "$work/.operations.XXXXXX") || return 1
  exec {operation_capture_read}< "$capture_path"
  exec {operation_capture_write}> "$capture_path"
  rm -f -- "$capture_path"
}

operation_capture_finish() {
  local phase=$1 joined
  local -a records=()
  exec {operation_capture_write}>&-
  mapfile -t records <&"$operation_capture_read"
  exec {operation_capture_read}>&-
  if (( ${#records[@]} )); then
    printf -v joined '%s\n' "${records[@]}"
    trace_data[$phase]=${joined%$'\n'}
  else
    trace_data[$phase]=
  fi
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
  rm -f -- "$result"
  payload_script="set -eu; test -d /usr; test -x /usr/bin/sh; test -d /host; test -d '/host$work'; test -d /host/run; test ! -e /host/proc/self; test -r '/host$work/fixtures/readable'; test -w '/host$work/fixtures/writable'; ! test -w '/host$work/fixtures/nonwritable'; ! test -x '/host$work/fixtures/group-only'; id -G | tr ' ' '\\n' | grep -qx '$caller_group';"
  if [[ -n $runtime_sentinel ]]; then
    payload_script+=" test -f '/host$runtime_sentinel/sentinel';"
  fi
  payload_script+=" printf host-projection-ok > '/host$result'"
  started=$(date +%s%N)
  verify_dispatch_integrity || return 1
  (
    exec 190>&"$operation_capture_write"
    exec 191>&"$probe_tag_fd"
    exec 193<&"$adapter_read"
    exec {operation_capture_read}>&-
    exec {operation_capture_write}>&-
    exec {probe_tag_fd}>&-
    exec {adapter_read}<&-
    exec {identity_capture_read}>&-
    exec {identity_capture_write}>&-
    PATH="$PATH" HOME="$work/home" CT_MOUNT_CFG="$work/missing-mount-config" \
      CT_HOST_PROJECTION_CACHE_ROOT="$work/cache" CT_MOUNT_PLAN_STATE_ROOT="$work/mount-plans" \
      CT_HOST_PROJECTION_RUNTIME_LABEL="${work##*/}" \
      CT_HOST_PROJECTION_RUNTIME_EXECUTABLE_FD="$runtime_executable_fd" \
      timeout --foreground --kill-after=2s 15s "$tool_root/ct_exec.sh" "--$backend" --ct-host-root "$mode" "${refresh_args[@]}" \
      "$immutable_image" /bin/sh -ec "$payload_script"
  ) >/dev/null 2>&1 || launch_status=$?
  foreground_elapsed[$phase]=$(elapsed_ms "$started")
  (( launch_status == 0 )) && [[ -f $result ]]
}

declare -A foreground_elapsed=()
overall_status=0
phase=cold
operation_capture_start
if ! run_foreground "$phase" auto 0; then overall_status=1; fi
operation_capture_finish "$phase"
if ! verify_dispatch_integrity; then
  overall_status=1
  mark_all fail "${dispatch_integrity_reason:-source-changed}"
  runner_finished=1
  emit_report failed "$report"
  exit 1
fi
if [[ $evidence_kind == fixture && ${CT_HOST_PROJECTION_RUNTIME_ABORT_AFTER_COLD:-} == 1 ]]; then
  false
fi

warm_elapsed=()
for ((sample = 1; sample <= warm_sample_count; sample++)); do
  phase="warm-$sample"
  operation_capture_start
  if ! run_foreground "$phase" auto 0; then overall_status=1; fi
  operation_capture_finish "$phase"
  warm_elapsed+=("${foreground_elapsed[$phase]:-0}")
done

operation_capture_start
required_refused=0
if ! run_foreground required required 0; then required_refused=1; fi
operation_capture_finish required

# A complete unchanged selection must be accepted without a probe. Refusal is
# exercised separately below by a deliberately partial cached selection.
selection_files=("$work/cache"/[0-9a-f][0-9a-f]*)
if [[ -f ${selection_files[0]:-} ]]; then
  mapfile -d '' -t partial_fields < "${selection_files[0]}" || overall_status=1
  if (( ${#partial_fields[@]} >= 7 )); then
    required_selection_completeness=${partial_fields[3]}
    partial_fields[3]=partial
    {
      for partial_field in "${partial_fields[@]}"; do printf '%s\0' "$partial_field"; done
    } > "$work/cache/.partial-selection" && mv -f -- "$work/cache/.partial-selection" "${selection_files[0]}" || overall_status=1
  else
    overall_status=1
  fi
else
  overall_status=1
fi
operation_capture_start
partial_required_refused=0
if ! run_foreground partial-required required 0; then partial_required_refused=1; fi
operation_capture_finish partial-required

operation_capture_start
if ! run_foreground refresh auto 1; then overall_status=1; fi
operation_capture_finish refresh

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

cold_operations=$(operation_json "${trace_data[cold]}")
refresh_operations=$(operation_json "${trace_data[refresh]}")
required_operations=$(operation_json "${trace_data[required]}")
warm_payloads=0
warm_clean=1
for ((sample = 1; sample <= warm_sample_count; sample++)); do
  current_operations=$(operation_json "${trace_data[warm-$sample]}")
  [[ $current_operations == *'"inspect":0,"probe_create":0,"probe_start":0,"probe_cleanup":0,"instance_liveness":0,"instance_start":0,"instance_stop":0,"payload":1'* ]] || warm_clean=0
  warm_payloads=$((warm_payloads + $(operation_count "${trace_data[warm-$sample]}" payload)))
done
warm_operations=$(printf '{"inspect":0,"probe_create":0,"probe_start":0,"probe_cleanup":0,"instance_liveness":0,"instance_start":0,"instance_stop":0,"payload":%s,"probe_tags":{"direct":0,"fallback":0}}' "$warm_payloads")

mapfile -t sorted_warm < <(printf '%s\n' "${warm_elapsed[@]}" | sort -n)
warm_median=${sorted_warm[$(((warm_sample_count - 1) / 2))]}
warm_p95=${sorted_warm[$(((warm_sample_count * 95 + 99) / 100 - 1))]}
warm_max=${sorted_warm[$((warm_sample_count - 1))]}
warm_timings=$(printf '{"cold_ms":%s,"refresh_ms":%s,"samples":%s,"median_ms":%s,"p95_ms":%s,"max_ms":%s}' "${foreground_elapsed[cold]}" "${foreground_elapsed[refresh]}" "$warm_sample_count" "$warm_median" "$warm_p95" "$warm_max")

expected_cold=0
case "$backend:$selection_strategy:$force_fallback" in
  docker:direct:0|podman:direct:0)
    expected_phase_operations='{"inspect":0,"probe_create":1,"probe_start":1,"probe_cleanup":1,"instance_liveness":0,"instance_start":0,"instance_stop":0,"payload":1,"probe_tags":{"direct":1,"fallback":0}}'
    ;;
  docker:fallback:*|podman:fallback:*)
    expected_phase_operations='{"inspect":0,"probe_create":2,"probe_start":1,"probe_cleanup":1,"instance_liveness":0,"instance_start":0,"instance_stop":0,"payload":1,"probe_tags":{"direct":1,"fallback":2}}'
    ;;
  singularity:fallback:*|apptainer:fallback:*)
    expected_phase_operations='{"inspect":0,"probe_create":0,"probe_start":1,"probe_cleanup":0,"instance_liveness":0,"instance_start":0,"instance_stop":0,"payload":1,"probe_tags":{"direct":0,"fallback":1}}'
    ;;
esac
[[ ${expected_phase_operations:-} == "$cold_operations" && $expected_phase_operations == "$refresh_operations" ]] && expected_cold=1
[[ $(operation_count "${trace_data[cold]}" payload) == 1 && $(operation_count "${trace_data[refresh]}" payload) == 1 ]] || expected_cold=0
[[ $warm_clean == 1 && $warm_payloads == "$warm_sample_count" ]] || expected_cold=0
if [[ ${required_selection_completeness:-partial} == complete ]]; then
  [[ $required_refused == 0 \
    && $required_operations == *'"inspect":0,"probe_create":0,"probe_start":0,"probe_cleanup":0,"instance_liveness":0,"instance_start":0,"instance_stop":0,"payload":1'* ]] || expected_cold=0
else
  [[ $required_refused == 1 && $required_operations == "$empty_operations" ]] || expected_cold=0
fi
[[ $partial_required_refused == 1 \
  && $(operation_json "${trace_data[partial-required]}") == "$empty_operations" ]] || expected_cold=0

cleanup_overall=exact
if [[ $backend == docker || $backend == podman ]]; then
  [[ $(operation_count "${trace_data[cold]}" probe-start) == $(operation_count "${trace_data[cold]}" probe-cleanup) ]] || cleanup_overall=failed
  [[ $(operation_count "${trace_data[refresh]}" probe-start) == $(operation_count "${trace_data[refresh]}" probe-cleanup) ]] || cleanup_overall=failed
fi
[[ $cleanup_overall == exact ]] || overall_status=1
phase_operations=$(printf '{"cold":%s,"warm":%s,"required":%s,"refresh":%s}' \
  "$cold_operations" "$warm_operations" "$required_operations" "$refresh_operations")

if [[ $overall_status == 0 && $expected_cold == 1 ]]; then
  set_case HP-HOST-001 pass selected-strategy "${foreground_elapsed[cold]}" "$cold_operations" "$empty_timings" exact
  set_case HP-HOST-002 pass run-and-kernel-exclusions "${foreground_elapsed[cold]}" "$cold_operations" "$empty_timings" exact
  set_case HP-HOST-003 pass caller-fixtures-and-groups "${foreground_elapsed[cold]}" "$cold_operations" "$empty_timings" exact
  set_case HP-HOST-004 pass cold-warm-refresh "$(( foreground_elapsed[cold] + foreground_elapsed[refresh] ))" "$cold_operations" "$warm_timings" exact "$phase_operations"
  if [[ ${required_selection_completeness:-partial} == complete ]]; then
    set_case HP-HOST-005 pass auto-required-exact-once "${foreground_elapsed[required]}" "$required_operations" "$empty_timings" exact
  else
    set_case HP-HOST-005 pass required-refused-before-payload "${foreground_elapsed[required]}" "$required_operations" "$empty_timings" exact
  fi
else
  overall_status=1
  set_case HP-HOST-001 fail matrix-incomplete 0 "$cold_operations" "$empty_timings" "$cleanup_overall"
  set_case HP-HOST-002 fail matrix-incomplete 0 "$cold_operations" "$empty_timings" "$cleanup_overall"
  set_case HP-HOST-003 fail matrix-incomplete 0 "$cold_operations" "$empty_timings" "$cleanup_overall"
  set_case HP-HOST-004 fail matrix-incomplete 0 "$cold_operations" "$warm_timings" "$cleanup_overall" "$phase_operations"
  set_case HP-HOST-005 fail matrix-incomplete 0 "$required_operations" "$empty_timings" "$cleanup_overall"
fi

case "$backend" in
  docker|podman)
    set_case HP-HOST-006 skip backend-inapplicable 0 "$empty_operations" "$empty_timings" not-needed
    ;;
  singularity|apptainer)
    persistent_started=$(date +%s%N)
    persistent_ok=1
    for phase in persistent-first persistent-reuse; do
      persistent_result="$work/results/$phase"
      rm -f -- "$persistent_result"
      operation_capture_start
      if verify_dispatch_integrity; then
        (
          exec 190>&"$operation_capture_write"
          exec 191>&"$probe_tag_fd"
          exec 192>&"$identity_capture_write"
          exec 193<&"$adapter_read"
          exec {operation_capture_read}>&-
          exec {operation_capture_write}>&-
          exec {probe_tag_fd}>&-
          exec {adapter_read}<&-
          exec {identity_capture_read}>&-
          exec {identity_capture_write}>&-
          PATH="$PATH" HOME="$work/home" CT_MOUNT_CFG="$work/missing-mount-config" \
            CT_HOST_PROJECTION_CACHE_ROOT="$work/cache" CT_MOUNT_PLAN_STATE_ROOT="$work/mount-plans" \
            CT_HOST_PROJECTION_RUNTIME_EXECUTABLE_FD="$runtime_executable_fd" \
            MKCHAD_TEST_RUNTIME_INSTANCES="$work/native-instances" \
            timeout --foreground --kill-after=2s 15s "$tool_root/ct_instance_exec.sh" "--$backend" --ct-instance-root "$work/persistent-instances" \
            --ct-host-root auto "$immutable_image" /bin/sh -ec "printf host-projection-ok > '/host$persistent_result'"
        ) >/dev/null 2>&1 || persistent_ok=0
      else
        persistent_ok=0
      fi
      operation_capture_finish "$phase"
      [[ -f $persistent_result ]] || persistent_ok=0
    done
    persistent_elapsed=$(elapsed_ms "$persistent_started")
    persistent_operations=$(operation_json "${trace_data[persistent-reuse]}")
    [[ $(operation_count "${trace_data[persistent-first]}" inspect) == 1 \
      && $(operation_count "${trace_data[persistent-first]}" instance-start) == 1 \
      && $(operation_count "${trace_data[persistent-first]}" payload) == 1 \
      && $(operation_count "${trace_data[persistent-first]}" probe-create) == 0 \
      && $(operation_count "${trace_data[persistent-first]}" probe-start) == 0 \
      && $(operation_count "${trace_data[persistent-first]}" probe-cleanup) == 0 ]] || persistent_ok=0
    [[ $persistent_operations == *'"inspect":0,"probe_create":0,"probe_start":0,"probe_cleanup":0,"instance_liveness":1,"instance_start":0,"instance_stop":0,"payload":1'* ]] || persistent_ok=0
    # The final reuse call is the last two adapter events in its own trace.
    mapfile -t persistent_events <<< "${trace_data[persistent-reuse]}"
    persistent_count=${#persistent_events[@]}
    (( persistent_count >= 2 )) && [[ ${persistent_events[persistent_count - 2]} == instance-liveness:* && ${persistent_events[persistent_count - 1]} == payload:* ]] || persistent_ok=0
    load_owned_instance || persistent_ok=0
    [[ $(operation_count "${trace_data[persistent-first]}" instance-start) == 1 ]] || persistent_ok=0
    cleanup_owned_instance || persistent_ok=0
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
[[ $(sha256sum -- "$runtime_path" | cut -d' ' -f1) == "$backend_executable_digest" ]] || integrity_reason=runtime-changed
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
if ! cleanup_runtime_sentinel; then
  cleanup_overall=failed
  overall_status=1
  mark_all fail cleanup-failed
fi

if [[ $overall_status == 0 ]]; then
  runner_finished=1
  emit_report passed "$report"
  exit 0
fi
runner_finished=1
emit_report failed "$report"
exit 1
