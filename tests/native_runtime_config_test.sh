#!/usr/bin/env bash
set -euo pipefail

work=${1:?pass a fresh task directory}
tool=${2:?pass the native executable}
runtime_child=${3:?pass the argv child probe}
buildx_child=${4:?pass the Buildx child probe}
timeout_tool=${5:?pass the test-only timeout executable}
work=$(realpath -m -- "$work")
[[ $work == /tmp/mkchad-v1/container-tools-c11/* && ! -e $work ]] || exit 2

home="$work/home"
config="$home/.config/ct_runtime.conf"
mkdir -p "$home/.config" "$work/fake-bin"
cat > "$config" <<EOF
# machine-local data, never shell input
CT_SINGULARITY_CACHE_DIR=$work/storage/singularity cache
CT_SINGULARITY_TMP_DIR=$work/storage/singularity tmp
CT_DOCKER_BUILD_CACHE_DIR=$work/storage/docker cache
CT_DOCKER_BUILD_TMP_DIR=$work/storage/docker tmp
EOF
chmod 600 "$config"

output="$work/runtime-child.out"
HOME="$home" CT_RUNTIME_CFG="$config" CT_TEST_OUTPUT="$output" \
  "$tool" runtime exec --backend apptainer -- "$runtime_child" 'space argument' 'semicolon;argument'
grep -Fx "cache=$work/storage/singularity cache" "$output"
grep -Fx "tmp=$work/storage/singularity tmp" "$output"
grep -Fx 'argc=3' "$output"
grep -Fx 'arg=space argument' "$output"
grep -Fx 'arg=semicolon;argument' "$output"
for directory in "$work/storage/singularity cache" "$work/storage/singularity tmp"; do
  [[ -d $directory && $(stat -Lc '%a' -- "$directory") == 700 ]]
done
override="$work/caller cache"
mkdir -m 700 "$override"
HOME="$home" CT_RUNTIME_CFG="$config" APPTAINER_CACHEDIR="$override" CT_TEST_OUTPUT="$output" \
  "$tool" runtime exec --backend apptainer -- "$runtime_child"
grep -Fx "cache=$override" "$output"
configured_override="$work/configured cache"
HOME="$home" CT_RUNTIME_CFG="$config" CT_SINGULARITY_CACHE_DIR="$configured_override" CT_TEST_OUTPUT="$output" \
  "$tool" runtime exec --backend apptainer -- "$runtime_child"
grep -Fx "cache=$configured_override" "$output"

set +e
HOME="$home" CT_RUNTIME_CFG="$config" CT_TEST_OUTPUT="$output" CT_TEST_EXIT=37 \
  "$tool" runtime exec --backend apptainer -- "$runtime_child" >/dev/null 2>&1
status=$?
set -e
[[ $status == 37 ]]
set +e
HOME="$home" CT_RUNTIME_CFG="$config" "$tool" runtime exec --backend apptainer -- "$work/missing-child" >/dev/null 2>&1
status=$?
set -e
[[ $status == 127 ]]
HOME="$home" CT_RUNTIME_CFG="$config" CT_TEST_OUTPUT="$output" CT_TEST_SLEEP=1 \
  "$tool" runtime exec --backend apptainer -- "$runtime_child" &
interrupted=$!
sleep 0.1
kill -TERM "$interrupted"
set +e
wait "$interrupted"
status=$?
set -e
[[ $status == 143 ]]

bad="$work/bad.conf"
for record in \
  'UNKNOWN_RUNTIME_KEY=/tmp/unsafe' \
  'CT_SINGULARITY_CACHE_DIR=relative/path' \
  'CT_DOCKER_BUILD_CACHE_DIR=/tmp/cache,unsafe' \
  $'CT_SINGULARITY_CACHE_DIR=/a\nCT_SINGULARITY_CACHE_DIR=/b'; do
  printf '%s\n' "$record" > "$bad"
  chmod 600 "$bad"
  if HOME="$home" CT_RUNTIME_CFG="$bad" "$tool" runtime exec --backend podman -- /bin/true; then
    printf '%s\n' 'native executable accepted malformed runtime config' >&2
    exit 1
  fi
done
printf 'CT_SINGULARITY_CACHE_DIR=/safe/cache\0UNKNOWN_RUNTIME_KEY=/unsafe\n' > "$bad"
chmod 600 "$bad"
if HOME="$home" CT_RUNTIME_CFG="$bad" "$tool" runtime exec --backend podman -- /bin/true; then
  printf '%s\n' 'native executable accepted an embedded NUL runtime config' >&2
  exit 1
fi
mkdir -m 777 "$work/unsafe-parent"
printf 'CT_SINGULARITY_CACHE_DIR=%s\n' "$work/unsafe-parent/cache" > "$bad"
chmod 600 "$bad"
if HOME="$home" CT_RUNTIME_CFG="$bad" "$tool" runtime exec --backend singularity -- /bin/true; then
  printf '%s\n' 'native executable adopted an unsafe storage root' >&2
  exit 1
fi
if "$tool" mount args "$work/missing-mount.conf"; then
  printf '%s\n' 'native executable accepted a missing mount configuration' >&2
  exit 1
fi

cp "$buildx_child" "$work/fake-bin/docker"
chmod 755 "$work/fake-bin/docker"
common=(HOME="$home" CT_RUNTIME_CFG="$config" PATH="$work/fake-bin:$PATH")
env "${common[@]}" "$tool" buildx exec --architecture x86_64 -- docker buildx build --tag image
current="$work/storage/docker cache/x86_64/current"
[[ -f $current/index.json ]]
env "${common[@]}" "$tool" buildx exec --architecture aarch64 -- docker buildx build --tag image
[[ -f "$work/storage/docker cache/aarch64/current/index.json" ]]
namespace="$work/storage/docker cache/x86_64"
mkdir "$namespace/.next.interrupted"
: > "$namespace/.next.interrupted/partial"
: > "$namespace/unrelated"
env "${common[@]}" "$tool" buildx exec --architecture x86_64 -- docker buildx build --tag recovered
[[ ! -e $namespace/.next.interrupted && -f $namespace/unrelated ]]

set +e
env "${common[@]}" CT_TEST_BUILDX_EXIT=19 "$tool" buildx exec --architecture x86_64 -- docker buildx build --tag failed
status=$?
set -e
[[ $status == 19 && -f $current/index.json ]]
set +e
env "${common[@]}" CT_TEST_BUILDX_NO_INDEX=1 "$tool" buildx exec --architecture x86_64 -- docker buildx build --tag no-index
status=$?
set -e
[[ $status != 0 && -f $current/index.json && -f $namespace/unrelated ]]
if compgen -G "$namespace/.next.*" >/dev/null; then
  printf '%s\n' 'failed Buildx commit retained a staging generation' >&2
  exit 1
fi
set +e
env "${common[@]}" CT_TEST_BUILDX_SIGNAL=1 "$tool" buildx exec --architecture x86_64 -- docker buildx build --tag signaled
status=$?
set -e
[[ $status == 143 && -f $current/index.json ]]
if env "${common[@]}" "$tool" buildx exec --architecture x86_64 -- docker buildx build --cache-to type=local,dest=unsafe; then
  printf '%s\n' 'native executable accepted caller cache transaction flags' >&2
  exit 1
fi

for malformed_timeout in '' NaN 1junk; do
  marker="$work/buildx-ran-$malformed_timeout"
  set +e
  env "${common[@]}" CT_DOCKER_BUILD_LOCK_TIMEOUT="$malformed_timeout" CT_TEST_BUILDX_RAN="$marker" \
    "$tool" buildx exec --architecture x86_64 -- docker buildx build --tag rejected
  status=$?
  set -e
  [[ $status != 0 && ! -e $marker && -f $current/index.json && -f $namespace/unrelated ]]
done

for malformed_timeout in '' NaN 1junk 0 0.0; do
  marker="$work/storage-ran-$malformed_timeout"
  set +e
  env "${common[@]}" CT_RUNTIME_STORAGE_TIMEOUT="$malformed_timeout" CT_TEST_BUILDX_RAN="$marker" \
    "$tool" buildx exec --architecture x86_64 -- docker buildx build --tag rejected
  status=$?
  set -e
  [[ $status != 0 && ! -e $marker && -f $current/index.json && -f $namespace/unrelated ]]
done

marker="$work/storage-timeout-ran"
set +e
env "${common[@]}" CT_RUNTIME_STORAGE_TIMEOUT=0.1 CT_TEST_STORAGE_TIMEOUT_FORCE=1 CT_TEST_BUILDX_RAN="$marker" \
  "$timeout_tool" buildx exec --architecture x86_64 -- docker buildx build --tag storage-timeout
status=$?
set -e
[[ $status != 0 && ! -e $marker && -f $current/index.json && -f $namespace/unrelated ]]

env "${common[@]}" CT_TEST_BUILDX_SLEEP=1 "$tool" buildx exec --architecture x86_64 -- docker buildx build --tag held &
holder=$!
sleep 0.1
set +e
env "${common[@]}" CT_DOCKER_BUILD_LOCK_TIMEOUT=0.05 "$tool" buildx exec --architecture x86_64 -- docker buildx build --tag contender
status=$?
set -e
wait "$holder"
[[ $status != 0 && -f $current/index.json ]]

printf '%s\n' 'container-tools native runtime configuration tests passed'
