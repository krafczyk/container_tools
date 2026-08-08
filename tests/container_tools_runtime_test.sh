#!/usr/bin/env bash
# Closed package/runtime evidence. Fixture reports prove this protocol only.
set -euo pipefail

readonly schema='container-tools.runtime-evidence/v1'
readonly work_root='/tmp/mkchad-v1/container-tools-c11/runtime-evidence'
script_dir=$(dirname "$(realpath "$0")")
tool_root=$(realpath "$script_dir/..")
host_runner="$script_dir/host_projection_runtime_test.sh"
source_commit=$(git -C "$tool_root" rev-parse HEAD 2>/dev/null || printf unknown)
architecture=$(uname -m | tr '[:upper:]' '[:lower:]')

usage() {
  printf '%s\n' 'usage: container_tools_runtime_test.sh --self-test'
  printf '%s\n' '       container_tools_runtime_test.sh --validate-report REPORT.json [--claimable]'
  printf '%s\n' '       container_tools_runtime_test.sh --run-parity --backend BACKEND --image IMAGE --package ARCHIVE --package-sha256 SHA256 --package-version VERSION --package-source-commit COMMIT --package-architecture ARCH --package-libc musl|glibc --bash-baseline REPORT --work FRESH_WORK'
  printf '%s\n' '       container_tools_runtime_test.sh --run-final-image --backend BACKEND --image IMAGE --package ARCHIVE --package-sha256 SHA256 --package-version VERSION --package-source-commit COMMIT --package-architecture ARCH --package-libc musl|glibc --container-release RELEASE.json --work FRESH_WORK'
}

source_manifest() { "$host_runner" --source-manifest; }

report_structure_valid() {
  local report=$1
  [[ -f $report && $(stat -c %s -- "$report" 2>/dev/null || printf 0) -le 1048576 ]] || return 1
  jq -s -e --arg schema "$schema" '
    def hex: type == "string" and test("^[0-9a-f]{64}$");
    def maybe_hex: . == "unavailable" or hex;
    def result: type == "string" and IN("pass", "fail", "skip");
    def release: type == "object" and (keys | sort) == ["digest", "product_major", "source_commit"] and
      (.digest | maybe_hex) and (.product_major | . == "unavailable" or (type == "number" and floor == . and . >= 1)) and
      (.source_commit | . == "unavailable" or (type == "string" and test("^[0-9a-f]{40}$")));
    length == 1 and (.[0] |
      type == "object" and
      (keys | sort) == ["architecture", "backend", "baseline", "cleanup", "evidence_kind", "image", "manifest_digest", "operations", "overall", "package", "runtime", "schema", "selected_root", "semantic_outcomes", "signals", "source_commit", "source_manifest"] and
      .schema == $schema and (.evidence_kind | IN("fixture", "native-parity", "final-image")) and
      (.overall | IN("passed", "failed", "unavailable")) and (.backend | IN("docker", "podman", "singularity", "apptainer")) and
      (.architecture | type == "string" and test("^[a-z0-9_.-]+$")) and
      (.source_commit | type == "string" and test("^[0-9a-f]{40}$")) and (.source_manifest | hex) and
      (.image | type == "object" and (keys | sort) == ["digest", "id", "role"] and (.digest | maybe_hex) and (.id | type == "string" and length <= 160) and (.role | IN("parity", "final", "fixture"))) and
      (.runtime | type == "object" and (keys | sort) == ["executable_digest", "version"] and (.executable_digest | maybe_hex) and (.version | type == "string" and length <= 160)) and
      (.package | type == "object" and (keys | sort) == ["archive_digest", "container", "host"] and (.archive_digest | maybe_hex) and (.host | release) and (.container | release)) and
      (.baseline | type == "object" and (keys | sort) == ["image_digest", "report_digest", "status"] and (.image_digest | maybe_hex) and (.report_digest | maybe_hex) and (.status | IN("accepted", "not-applicable", "unavailable"))) and
      (.manifest_digest | maybe_hex) and
      (.operations | type == "object" and (keys | sort) == ["manifest_publish", "nested_dispatch", "outer_launch", "payload"] and all(.[]; type == "number" and floor == . and . >= 0)) and
      (.semantic_outcomes | type == "object" and (keys | sort) == ["manifest", "nested", "outer", "persistent", "selected_root", "status"] and all(.[]; result)) and
      (.selected_root | type == "object" and (keys | sort) == ["alternate", "default"] and all(.[]; result)) and
      (.signals | type == "object" and (keys | sort) == ["payload_exit", "signal"] and (.payload_exit | type == "number" and floor == . and . >= 0) and (.signal | IN("none", "HUP", "INT", "TERM"))) and
      (.cleanup | type == "object" and (keys | sort) == ["owned_residue", "status"] and (.owned_residue | type == "number" and floor == . and . >= 0) and (.status | IN("exact", "failed", "not-needed")))
    )
  ' "$report" >/dev/null 2>&1
}

validate_report() {
  local report=$1 claimable=${2:-0} manifest evidence_kind overall
  report_structure_valid "$report" || return 2
  manifest=$(source_manifest) || return 1
  evidence_kind=$(jq -r '.evidence_kind' "$report")
  overall=$(jq -r '.overall' "$report")
  [[ $overall == unavailable ]] && return 77
  [[ $claimable == 1 && $evidence_kind == fixture ]] && return 77
  [[ $overall == passed ]] || return 1
  jq -e --arg commit "$source_commit" --arg manifest "$manifest" --argjson claimable "$claimable" '
    .source_commit == $commit and .source_manifest == $manifest and
    (if .evidence_kind == "native-parity" then
      .image.role == "parity" and .baseline.status == "accepted" and .baseline.image_digest == .image.digest and
      (.baseline.report_digest | test("^[0-9a-f]{64}$")) and .package.host.source_commit == .source_commit and .package.container.digest == "unavailable"
    elif .evidence_kind == "final-image" then
      .image.role == "final" and .baseline == {status:"not-applicable",report_digest:"unavailable",image_digest:"unavailable"} and
      .package.host.source_commit == .source_commit and .package.host.product_major == .package.container.product_major and .package.container.digest != "unavailable"
    else true end) and
    (if $claimable == 1 then .evidence_kind != "fixture" else true end) and
    .manifest_digest != "unavailable" and .runtime.executable_digest != "unavailable" and .package.archive_digest != "unavailable" and
    .cleanup == {status:"exact",owned_residue:0} and (.operations | all(.[]; . >= 1)) and
    (.semantic_outcomes | all(. == "pass")) and (.selected_root | all(. == "pass")) and .signals == {payload_exit:0,signal:"none"}
  ' "$report" >/dev/null 2>&1
}

emit_fixture() {
  local report=$1
  jq -n --arg schema "$schema" --arg architecture "$architecture" --arg commit "$source_commit" --arg manifest "$(source_manifest)" '
    {schema:$schema,evidence_kind:"fixture",overall:"passed",backend:"docker",architecture:$architecture,source_commit:$commit,source_manifest:$manifest,
     image:{digest:"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",id:"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",role:"fixture"},runtime:{executable_digest:"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",version:"fixture"},
     package:{archive_digest:"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",host:{digest:"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",product_major:1,source_commit:$commit},container:{digest:"unavailable",product_major:"unavailable",source_commit:"unavailable"}},baseline:{status:"not-applicable",report_digest:"unavailable",image_digest:"unavailable"},manifest_digest:"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",operations:{outer_launch:1,manifest_publish:1,nested_dispatch:1,payload:1},semantic_outcomes:{outer:"pass",manifest:"pass",nested:"pass",persistent:"pass",selected_root:"pass",status:"pass"},selected_root:{default:"pass",alternate:"pass"},signals:{payload_exit:0,signal:"none"},cleanup:{status:"exact",owned_residue:0}}' > "$report"
}

self_test() {
  local work report bad parity final result
  work="$work_root/self-test"
  report="$work/report.json"
  bad="$work/bad.json"
  parity="$work/parity.json"
  final="$work/final.json"
  rm -rf -- "$work"
  mkdir -p -- "$work"
  trap 'rm -rf -- "$work"' RETURN
  emit_fixture "$report"
  validate_report "$report"
  set +e
  validate_report "$report" 1
  result=$?
  set -e
  [[ $result == 77 ]] || return 1
  jq '.evidence_kind = "native-parity" | .image.role = "parity" |
      .baseline = {status:"accepted",report_digest:"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",image_digest:.image.digest}' \
    "$report" > "$parity"
  validate_report "$parity" 1
  jq '.baseline.image_digest = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"' "$parity" > "$bad"
  set +e
  validate_report "$bad" 1
  result=$?
  set -e
  [[ $result == 1 ]] || return 1
  jq '.evidence_kind = "final-image" | .image.role = "final" |
      .baseline = {status:"not-applicable",report_digest:"unavailable",image_digest:"unavailable"} |
      .package.container = {digest:"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",product_major:1,source_commit:.source_commit}' \
    "$report" > "$final"
  validate_report "$final" 1
  jq 'del(.cleanup)' "$report" > "$bad"
  set +e
  validate_report "$bad"
  result=$?
  set -e
  [[ $result == 2 ]] || return 1
  jq '.cleanup.owned_residue = 1' "$report" > "$bad"
  set +e
  validate_report "$bad"
  result=$?
  set -e
  [[ $result == 1 ]] || return 1
  jq '.evidence_kind = "native-parity"' "$report" > "$bad"
  set +e
  validate_report "$bad" 1
  result=$?
  set -e
  [[ $result == 1 ]] || return 1
  printf '%s\n' 'container-tools runtime report tests passed'
}

release_fields() {
  jq -e 'type == "object" and .schema == "container-tools.release/v1" and (.product_major | type == "number" and floor == . and . >= 1) and (.source_commit | type == "string" and test("^[0-9a-f]{40}$"))' "$1" >/dev/null
}

mode=''
report=''
claimable=0
backend=''
image=''
package=''
package_sha256=''
package_version=''
package_source_commit=''
package_architecture=''
package_libc=''
bash_baseline=''
container_release=''
work=''
while (($#)); do
  case $1 in
    --self-test|--run-parity|--run-final-image) [[ -z $mode ]] || { usage >&2; exit 2; }; mode=${1#--}; shift ;;
    --validate-report) [[ -z $mode && $# -ge 2 ]] || { usage >&2; exit 2; }; mode=validate; report=$2; shift 2 ;;
    --claimable) claimable=1; shift ;;
    --backend|--image|--package|--package-sha256|--package-version|--package-source-commit|--package-architecture|--package-libc|--bash-baseline|--container-release|--work)
      (($# >= 2)) || { usage >&2; exit 2; }
      case $1 in
        --backend) backend=$2 ;; --image) image=$2 ;; --package) package=$2 ;; --package-sha256) package_sha256=$2 ;;
        --package-version) package_version=$2 ;; --package-source-commit) package_source_commit=$2 ;; --package-architecture) package_architecture=$2 ;; --package-libc) package_libc=$2 ;;
        --bash-baseline) bash_baseline=$2 ;; --container-release) container_release=$2 ;; --work) work=$2 ;;
      esac
      shift 2 ;;
    *) usage >&2; exit 2 ;;
  esac
done
command -v jq >/dev/null 2>&1 || exit 77
case $mode in
  self-test) [[ $claimable == 0 ]] || { usage >&2; exit 2; }; self_test; exit ;;
  validate) [[ -n $report ]] || { usage >&2; exit 2; }; validate_report "$report" "$claimable"; exit $? ;;
  run-parity|run-final-image) ;;
  *) usage >&2; exit 2 ;;
esac

# Real runs are deliberately admitted only after every immutable input is present.
[[ $backend =~ ^(docker|podman|singularity|apptainer)$ && -n $image && -f $package && $package_sha256 =~ ^[0-9a-f]{64}$ &&
  $package_version =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$ && $package_source_commit =~ ^[0-9a-f]{40}$ &&
  $package_architecture =~ ^[a-z0-9_.-]+$ && $package_libc =~ ^(musl|glibc)$ && $work == "$work_root"/* && $work != "$work_root"/*/* && ! -e $work ]] || { usage >&2; exit 2; }
if [[ $mode == run-parity ]]; then
  [[ -f $bash_baseline ]] || { printf '%s\n' 'native parity unavailable: retained Bash baseline is missing' >&2; exit 77; }
  "$host_runner" --validate-retained-bash-baseline-report "$bash_baseline" || exit $?
else
  if [[ ! -f $container_release ]] || ! release_fields "$container_release"; then
    printf '%s\n' 'final-image operability unavailable: in-container package identity is missing or malformed' >&2
    exit 77
  fi
fi
command -v "$backend" >/dev/null 2>&1 || { printf 'native %s unavailable: runtime executable is absent\n' "$backend" >&2; exit 77; }
printf '%s\n' 'native runtime invocation requires the retained image and release-specific selected-root harness' >&2
exit 77
