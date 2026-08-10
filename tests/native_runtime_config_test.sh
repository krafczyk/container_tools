#!/usr/bin/env bash
set -euo pipefail

work=${1:?pass a fresh task directory}
tool=${2:?pass the native executable}
work=$(realpath -m -- "$work")
[[ $work == /tmp/mkchad-v1/container-tools-runtime-boundary/* && ! -e $work ]] || exit 2

home="$work/home"
config="$home/.config/ct_runtime.conf"
image="$work/image.sif"
mkdir -p "$home/.config"
printf 'fixture\n' > "$image"
printf 'CT_SINGULARITY_CACHE_DIR=%s/cache\nCT_SINGULARITY_TMP_DIR=%s/tmp\n' \
  "$work" "$work" > "$config"
chmod 600 "$config"

HOME="$home" CT_RUNTIME_CFG="$config" CT_DRY_RUN=1 "$tool" instance exec --apptainer \
  --ct-instance-root "$work/instances" -- "$image" /bin/true >/dev/null
[[ -d $work/cache && -d $work/tmp ]]

removed_key="CT_"; removed_key+="DOCKER_"; removed_key+="BUILD_CACHE_DIR=$work/cache"
printf '%s\n' "$removed_key" > "$config"
if HOME="$home" CT_RUNTIME_CFG="$config" CT_DRY_RUN=1 "$tool" instance exec --apptainer \
  --ct-instance-root "$work/instances" -- "$image" /bin/true >/dev/null 2>&1; then
  printf '%s\n' 'runtime accepted a removed configuration key' >&2
  exit 1
fi

printf '%s\n' 'container-tools native runtime configuration tests passed'
