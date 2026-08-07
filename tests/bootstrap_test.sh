#!/usr/bin/env bash
set -euo pipefail

work=${1:?pass a task-specific directory beneath /tmp/mkchad-v1/host-root-projection}
root=${2:?pass the container_tools checkout}
root=$(realpath "$root")
[[ $work == /tmp/mkchad-v1/host-root-projection/* ]] || { printf '%s\n' 'test directory must be beneath /tmp/mkchad-v1/host-root-projection' >&2; exit 2; }
[[ ! -e $work ]] || { printf '%s\n' 'test directory already exists' >&2; exit 2; }

fake="$work/fake-bin"
home="$work/home"
log="$work/backend.log"
bootstrap="$work/bootstrap with spaces"
image="$work/image.sif"
mkdir -p "$fake" "$home"
: > "$image"
cat > "$bootstrap" <<'EOF'
#!/usr/bin/env bash
exec "$@"
EOF
cat > "$fake/apptainer" <<'EOF'
#!/usr/bin/env bash
for argument; do
  if [[ $argument == /bin/sh ]]; then
    case "$0" in
      *docker|*podman) printf '%s\n' 'ct-host-projection-group=none' ;;
      *) printf '%s\n' 'ct-host-projection-group=native' ;;
    esac
    exit 0
  fi
done
printf '%s\n' "$@" > "$CT_TEST_LOG"
exit "${CT_TEST_STATUS:-0}"
EOF
cat > "$fake/docker" <<'EOF'
#!/usr/bin/env bash
case "${1:-}" in
  create)
    printf '%s\n' fake-container-id
    ;;
  start)
    case "$0" in
      *docker) printf '%s\n' 'ct-host-projection-group=numeric' ;;
      *) printf '%s\n' 'ct-host-projection-group=keep' ;;
    esac
    ;;
  rm) ;;
  *)
    printf '%s\n' "$@" > "$CT_TEST_LOG"
    exit "${CT_TEST_STATUS:-0}"
    ;;
esac
EOF
cp "$fake/apptainer" "$fake/singularity"
cp "$fake/docker" "$fake/podman"
chmod 755 "$bootstrap" "$fake/apptainer" "$fake/singularity" "$fake/docker" "$fake/podman"

common_env=(HOME="$home" PATH="$fake:$PATH" CT_TEST_LOG="$log" CT_MOUNT_CFG="$work/missing-mount-config" CT_HOST_PROJECTION_CACHE_ROOT="$work/cache")

assert_manifest_bind() {
  local backend=$1 require_bootstrap=${2:-0} index source='' digest field bytes=0 LC_ALL=C
  local -a fields=()
  for ((index = 0; index < ${#argv[@]}; index++)); do
    if [[ ${argv[index]} == --bind && $((index + 1)) -lt ${#argv[@]} \
      && ${argv[index + 1]} == *:/.container-tools-mount-plan:ro ]]; then
      source=${argv[index + 1]%:/.container-tools-mount-plan:ro}
    elif [[ ${argv[index]} == --mount && $((index + 1)) -lt ${#argv[@]} \
      && ${argv[index + 1]} == type=bind,source=*,target=/.container-tools-mount-plan,readonly ]]; then
      source=${argv[index + 1]#type=bind,source=}
      source=${source%,target=/.container-tools-mount-plan,readonly}
    fi
  done
  [[ -n $source && -f $source ]] || { printf 'missing %s mount-plan bind\n' "$backend" >&2; exit 1; }
  mapfile -d '' -t fields < "$source"
  [[ ${fields[0]} == ct-mount-plan-v1 && ${fields[1]} =~ ^[0-9a-f]{64}$ \
    && ${fields[2]} == "$backend" && ${fields[6]} =~ ^[0-9]+$ ]] || {
    printf 'invalid %s mount-plan record\n' "$backend" >&2
    exit 1
  }
  (( ${#fields[@]} == 7 + 5 * 10#${fields[6]} )) || {
    printf 'invalid %s mount-plan record count\n' "$backend" >&2
    exit 1
  }
  digest=$({ printf '%s\0' "${fields[0]}"; printf '%s\0' "${fields[@]:2}"; } | sha256sum)
  digest=${digest%% *}
  [[ $digest == "${fields[1]}" && ${source##*/} == "$digest.manifest" ]] || {
    printf 'invalid %s content-addressed mount-plan source\n' "$backend" >&2
    exit 1
  }
  for field in "${fields[@]}"; do
    bytes=$((bytes + ${#field} + 1))
  done
  (( bytes == $(stat -c '%s' -- "$source") )) || {
    printf 'invalid %s mount-plan framing\n' "$backend" >&2
    exit 1
  }
  if (( require_bootstrap )); then
    local found=0
    for ((index = 7; index < ${#fields[@]}; index += 5)); do
      [[ ${fields[index]} == bootstrap-internal \
        && ${fields[index + 1]} == /.container-tools-bootstrap \
        && ${fields[index + 2]} == /.container-tools-bootstrap \
        && ${fields[index + 3]} == read-only \
        && ${fields[index + 4]} == runtime-default ]] && found=1
    done
    (( found )) || { printf 'missing %s bootstrap manifest tuple\n' "$backend" >&2; exit 1; }
  fi
}

env "${common_env[@]}" "$root/ct_exec.sh" --apptainer \
  --ct-env 'FEATURE=value with spaces' --ct-bootstrap "$bootstrap" -- \
  "$image" command 'argument with spaces'
mapfile -t argv < "$log"
assert_manifest_bind apptainer 1
[[ ${argv[0]} == exec && ${argv[1]} == --pwd ]] || { printf '%s\n' 'apptainer exec prefix changed' >&2; exit 1; }
[[ " ${argv[*]} " == *" --bind $bootstrap:/.container-tools-bootstrap:ro "* ]] || { printf '%s\n' 'bootstrap was not mounted read-only' >&2; exit 1; }
[[ " ${argv[*]} " == *" --env FEATURE=value with spaces "* ]] || { printf '%s\n' 'normalized environment was not forwarded' >&2; exit 1; }
count=${#argv[@]}
[[ ${argv[count-4]} == "$image" && ${argv[count-3]} == /.container-tools-bootstrap \
  && ${argv[count-2]} == command && ${argv[count-1]} == 'argument with spaces' ]] || {
  printf '%s\n' 'bootstrap exec payload changed' >&2; exit 1;
}

env "${common_env[@]}" "$root/ct_shell.sh" --apptainer \
  --ct-bootstrap "$bootstrap" --ct-container-shell /bin/bash -- "$image"
mapfile -t argv < "$log"
assert_manifest_bind apptainer 1
count=${#argv[@]}
[[ ${argv[0]} == exec && ${argv[count-4]} == "$image" \
  && ${argv[count-3]} == /.container-tools-bootstrap \
  && ${argv[count-2]} == /bin/bash && ${argv[count-1]} == -i ]] || {
  printf '%s\n' 'bootstrap shell payload changed' >&2; exit 1;
}

env "${common_env[@]}" "$root/ct_exec.sh" --docker \
  --ct-bootstrap "$bootstrap" -- "$image" command
mapfile -t argv < "$log"
assert_manifest_bind docker 1
[[ ${argv[0]} == run && ${argv[1]} == --rm ]] || { printf '%s\n' 'docker bootstrap did not use a normalized run invocation' >&2; exit 1; }
[[ " ${argv[*]} " == *" type=bind,source=$bootstrap,target=/.container-tools-bootstrap,readonly "* ]] || {
  printf '%s\n' 'docker bootstrap mount changed' >&2; exit 1;
}

env "${common_env[@]}" "$root/ct_exec.sh" --apptainer \
  --env LEGACY=value "$image" command
mapfile -t argv < "$log"
assert_manifest_bind apptainer
count=${#argv[@]}
[[ ${argv[0]} == exec && ${argv[count-4]} == --env \
  && ${argv[count-3]} == LEGACY=value && ${argv[count-2]} == "$image" \
  && ${argv[count-1]} == command ]] || {
  printf '%s\n' 'legacy backend arguments changed without a bootstrap' >&2; exit 1;
}

env "${common_env[@]}" "$root/ct_exec.sh" --apptainer \
  "$image" command -- 'payload option'
mapfile -t argv < "$log"
assert_manifest_bind apptainer
count=${#argv[@]}
[[ ${argv[count-4]} == "$image" && ${argv[count-3]} == command \
  && ${argv[count-2]} == -- && ${argv[count-1]} == 'payload option' ]] || {
  printf '%s\n' 'legacy payload delimiter moved or disappeared' >&2; exit 1;
}

env "${common_env[@]}" "$root/ct_exec.sh" --singularity \
  --ct-bootstrap "$bootstrap" -- "$image" command
mapfile -t argv < "$log"
assert_manifest_bind singularity 1
[[ ${argv[0]} == exec ]] || { printf '%s\n' 'singularity bootstrap did not use exec' >&2; exit 1; }

env "${common_env[@]}" "$root/ct_shell.sh" --podman \
  --ct-bootstrap "$bootstrap" -- "$image"
mapfile -t argv < "$log"
assert_manifest_bind podman 1
[[ ${argv[0]} == run && ${argv[1]} == --rm && ${argv[2]} == -it ]] || {
  printf '%s\n' 'podman bootstrap shell was not normalized' >&2; exit 1;
}

set +e
env "${common_env[@]}" CT_TEST_STATUS=37 "$root/ct_exec.sh" --apptainer "$image" command
backend_status=$?
set -e
[[ $backend_status -eq 37 ]] || { printf '%s\n' 'backend exit status was not preserved' >&2; exit 1; }

set +e
error=$(env "${common_env[@]}" "$root/ct_exec.sh" --apptainer --ct-bootstrap "$bootstrap" "$image" command 2>&1)
status=$?
set -e
[[ $status -ne 0 && $error == *"requires '--'"* ]] || { printf '%s\n' 'ambiguous bootstrap payload was accepted' >&2; exit 1; }

set +e
# shellcheck disable=SC2016 # The literal substitution syntax must be rejected.
error=$(env "${common_env[@]}" "$root/ct_exec.sh" --apptainer \
  --ct-env 'UNSAFE=$(date)' "$image" command 2>&1)
status=$?
set -e
[[ $status -ne 0 && $error == *'invalid --ct-env'* ]] || {
  printf '%s\n' 'shell-evaluated environment value was accepted' >&2; exit 1;
}

printf '%s\n' 'container-tools bootstrap tests passed'
