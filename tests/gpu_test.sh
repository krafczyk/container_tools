#!/usr/bin/env bash
set -euo pipefail
tool=${1:?pass the native container-tools executable}
export CT_DRY_RUN=1
for backend in apptainer singularity; do
  for operation in exec shell; do
    output=$("$tool" "$operation" "--$backend" --ct-gpu nvidia -- image.sif)
    [[ $output == "$backend $operation --nv --pwd "* ]] || exit 1
    output=$("$tool" "$operation" "--$backend" --ct-gpu none -- image.sif)
    [[ $output == "$backend $operation --pwd "* && $output != *' --nv '* ]] || exit 1
    default=$("$tool" "$operation" "--$backend" -- image.sif)
    [[ $output == "$default" ]] || exit 1
    for mode in '' amd rocm invalid; do
      status=0
      "$tool" "$operation" "--$backend" --ct-gpu "$mode" -- image.sif >/dev/null 2>&1 || status=$?
      [[ $status == 64 ]] || exit 1
    done
    status=0
    "$tool" "$operation" "--$backend" --ct-gpu >/dev/null 2>&1 || status=$?
    [[ $status == 64 ]] || exit 1
    status=0
    "$tool" "$operation" "--$backend" --ct-gpu nvidia --ct-gpu none -- image.sif >/dev/null 2>&1 || status=$?
    [[ $status == 64 ]] || exit 1
  done
  for mode in '' amd invalid; do
    status=0
    "$tool" instance exec "--$backend" --ct-instance-root /nonexistent/gpu-test --ct-gpu "$mode" -- image.sif true >/dev/null 2>&1 || status=$?
    [[ $status == 1 ]] || exit 1
  done
  status=0
  "$tool" instance exec "--$backend" --ct-instance-root /nonexistent/gpu-test --ct-gpu nvidia --ct-gpu none -- image.sif true >/dev/null 2>&1 || status=$?
  [[ $status == 1 ]] || exit 1
  status=0
  "$tool" instance exec "--$backend" --ct-instance-root /nonexistent/gpu-test --ct-gpu >/dev/null 2>&1 || status=$?
  [[ $status == 1 ]] || exit 1
done
for backend in docker podman; do
  status=0
  "$tool" exec "--$backend" --ct-gpu nvidia -- image.sif true >/dev/null 2>&1 || status=$?
  [[ $status == 64 ]] || exit 1
done
printf '%s\n' 'GPU option parsing and native exec/shell argument placement passed'
