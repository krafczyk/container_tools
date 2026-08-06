#!/usr/bin/env bash
# Cumulative, operator-run evidence for foreground host-root projection. It is
# intentionally opt-in: deterministic tests exercise this frontend with fake
# clients, while real backend claims require the emitted host report.
set -euo pipefail

readonly schema='container-tools.host-projection-runtime/v1'
readonly work_root='/tmp/mkchad-v1/host-root-projection-host'
script_dir=$(dirname "$(realpath "$0")")
tool_root=$(realpath "$script_dir/..")
source_commit=$(git -C "$tool_root" rev-parse HEAD 2>/dev/null || printf '%s' unknown)
architecture=$(uname -m)

usage() {
  printf '%s\n' 'usage: host_projection_runtime_test.sh --backend docker|podman|singularity|apptainer --image LOCAL_IMAGE --work /tmp/mkchad-v1/host-root-projection-host/RUN_ID'
}

json_string() {
  local value=$1
  value=${value//\\/\\\\}
  value=${value//\"/\\\"}
  value=${value//$'\n'/}
  printf '"%s"' "$value"
}

source_manifest() {
  local file digest
  {
    for file in ct_library.sh ct_exec.sh ct_shell.sh tests/bootstrap_test.sh tests/host_projection_test.sh tests/host_projection_runtime_test.sh; do
      digest=$(sha256sum "$tool_root/$file")
      printf '%s\0%s\0' "$file" "${digest%% *}"
    done
  } | sha256sum | cut -d' ' -f1
}

emit_report() {
  local overall=$1 reason=$2 report=${3:-} manifest=$4 image_digest=$5
  local case_state=pass
  case "$overall" in
    unavailable) case_state=skip ;;
    failed) case_state=fail ;;
  esac
  {
    printf '{"schema":'
    json_string "$schema"
    printf ',"overall":'
    json_string "$overall"
    printf ',"backend":'
    json_string "$backend"
    printf ',"architecture":'
    json_string "$architecture"
    printf ',"source_commit":'
    json_string "$source_commit"
    printf ',"source_manifest":'
    json_string "$manifest"
    printf ',"image_digest":'
    json_string "$image_digest"
    printf ',"reason":'
    json_string "$reason"
    printf ',"cleanup":"not-needed","cases":['
    local case_id separator=
    for case_id in HP-HOST-001 HP-HOST-002 HP-HOST-003 HP-HOST-004 HP-HOST-005; do
      printf '%s{"id":' "$separator"
      json_string "$case_id"
      printf ',"status":'
      json_string "$case_state"
      printf ',"reason":'
      json_string "$reason"
      printf ',"elapsed_ms":0,"operations":{"inspect":0,"probe":0,"payload":0}}'
      separator=,
    done
    printf ']}\n'
  } | if [[ -n $report ]]; then tee "$report"; else tee /dev/stderr | true; fi
}

backend=
image=
work=
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
    *) usage >&2; exit 2 ;;
  esac
done

case "$backend" in docker|podman|singularity|apptainer) ;; *) usage >&2; exit 2 ;; esac
[[ -n $image && -n $work && $work == "$work_root"/* && $work != "$work_root"/*/* \
  && ${work##*/} =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ && ! -e $work ]] || {
  usage >&2
  exit 2
}
if [[ $backend == singularity || $backend == apptainer ]]; then
  [[ $image == /* && -f $image ]] || { usage >&2; exit 2; }
fi

# A creation failure is environmental rather than malformed input. It cannot
# write a report file, but stdout still records the unavailable gate.
if ! mkdir -p -- "$work_root" || ! mkdir -- "$work"; then
  manifest=$(source_manifest)
  emit_report unavailable work-create-failed '' "$manifest" unavailable
  exit 77
fi
report="$work/report.json"
manifest_before=$(source_manifest)
if [[ ${CT_HOST_PROJECTION_RUNTIME_TEST:-} != 1 ]]; then
  emit_report unavailable disabled "$report" "$manifest_before" unavailable
  exit 77
fi

if ! command -v -- "$backend" >/dev/null 2>&1; then
  emit_report unavailable runtime-unavailable "$report" "$manifest_before" unavailable
  exit 77
fi

immutable_image=
case "$backend" in
  docker|podman)
    # `image inspect` addresses local storage only; this runner never pulls.
    immutable_image=$("$backend" image inspect --format '{{.Id}}' "$image" 2>/dev/null) || {
      emit_report unavailable local-image-unavailable "$report" "$manifest_before" unavailable
      exit 77
    }
    [[ -n $immutable_image ]] || {
      emit_report unavailable local-image-unavailable "$report" "$manifest_before" unavailable
      exit 77
    }
    image_digest=$(printf '%s' "$immutable_image" | sha256sum | cut -d' ' -f1)
    ;;
  singularity|apptainer)
    immutable_image="$work/image.sif"
    cp --reflink=auto -- "$image" "$immutable_image" || {
      emit_report unavailable image-stage-failed "$report" "$manifest_before" unavailable
      exit 77
    }
    image_digest=$(sha256sum "$immutable_image" | cut -d' ' -f1)
    ;;
esac

manifest_after_admission=$(source_manifest)
[[ $manifest_before == "$manifest_after_admission" ]] || {
  emit_report failed source-changed "$report" "$manifest_after_admission" "$image_digest"
  exit 1
}

# U2 executes one cold foreground launch, one warm launch, and one refresh
# launch. The payload only writes beneath this run's work directory. The runner
# records no argv, output, environment, host paths, or mount inventory.
backend_flag="--$backend"
payload_path="/host$work/payload-sentinel"
status=0
for launch_phase in cold warm refresh; do
  host_root_argv=(--ct-host-root auto)
  [[ $launch_phase == refresh ]] && host_root_argv+=(--ct-host-root-refresh)
  CT_HOST_PROJECTION_CACHE_ROOT="$work/cache" CT_HOST_PROJECTION_RUNTIME_LABEL="${work##*/}" \
    "$tool_root/ct_exec.sh" "$backend_flag" \
    "${host_root_argv[@]}" "$immutable_image" /bin/sh -c "printf x >> '$payload_path'" >/dev/null 2>&1 || status=1
done

manifest_after=$(source_manifest)
case "$backend" in
  docker|podman)
    current_image=$("$backend" image inspect --format '{{.Id}}' "$image" 2>/dev/null || true)
    [[ $current_image == "$immutable_image" ]] || status=1
    ;;
  singularity|apptainer)
    [[ $(sha256sum "$immutable_image" | cut -d' ' -f1) == "$image_digest" ]] || status=1
    ;;
esac
[[ $manifest_after == "$manifest_before" ]] || status=1

if (( status == 0 )); then
  emit_report passed passed "$report" "$manifest_after" "$image_digest"
  exit 0
fi
emit_report failed foreground-check-failed "$report" "$manifest_after" "$image_digest"
exit 1
