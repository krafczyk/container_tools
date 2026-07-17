#!/usr/bin/env bash
set -euo pipefail

work=${1:?pass a task-specific directory beneath /tmp/opencode-mkchad}
root=${2:?pass the container_tools checkout}
root=$(realpath "$root")
[[ $work == /tmp/opencode-mkchad/* ]] || { printf '%s\n' 'test directory must be beneath /tmp/opencode-mkchad' >&2; exit 2; }
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
printf '%s\n' "$@" > "$CT_TEST_LOG"
exit "${CT_TEST_STATUS:-0}"
EOF
cat > "$fake/docker" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$CT_TEST_LOG"
exit "${CT_TEST_STATUS:-0}"
EOF
cp "$fake/apptainer" "$fake/singularity"
cp "$fake/docker" "$fake/podman"
chmod 755 "$bootstrap" "$fake/apptainer" "$fake/singularity" "$fake/docker" "$fake/podman"

common_env=(HOME="$home" PATH="$fake:$PATH" CT_TEST_LOG="$log" CT_MOUNT_CFG="$work/missing-mount-config")

env "${common_env[@]}" "$root/ct_exec.sh" --apptainer \
  --ct-env 'FEATURE=value with spaces' --ct-bootstrap "$bootstrap" -- \
  "$image" command 'argument with spaces'
mapfile -t argv < "$log"
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
count=${#argv[@]}
[[ ${argv[0]} == exec && ${argv[count-4]} == "$image" \
  && ${argv[count-3]} == /.container-tools-bootstrap \
  && ${argv[count-2]} == /bin/bash && ${argv[count-1]} == -i ]] || {
  printf '%s\n' 'bootstrap shell payload changed' >&2; exit 1;
}

env "${common_env[@]}" "$root/ct_exec.sh" --docker \
  --ct-bootstrap "$bootstrap" -- "$image" command
mapfile -t argv < "$log"
[[ ${argv[0]} == run && ${argv[1]} == --rm ]] || { printf '%s\n' 'docker bootstrap did not use a normalized run invocation' >&2; exit 1; }
[[ " ${argv[*]} " == *" type=bind,source=$bootstrap,target=/.container-tools-bootstrap,readonly "* ]] || {
  printf '%s\n' 'docker bootstrap mount changed' >&2; exit 1;
}

env "${common_env[@]}" "$root/ct_exec.sh" --apptainer \
  --env LEGACY=value "$image" command
mapfile -t argv < "$log"
count=${#argv[@]}
[[ ${argv[0]} == exec && ${argv[count-4]} == --env \
  && ${argv[count-3]} == LEGACY=value && ${argv[count-2]} == "$image" \
  && ${argv[count-1]} == command ]] || {
  printf '%s\n' 'legacy backend arguments changed without a bootstrap' >&2; exit 1;
}

env "${common_env[@]}" "$root/ct_exec.sh" --apptainer \
  "$image" command -- 'payload option'
mapfile -t argv < "$log"
count=${#argv[@]}
[[ ${argv[count-4]} == "$image" && ${argv[count-3]} == command \
  && ${argv[count-2]} == -- && ${argv[count-1]} == 'payload option' ]] || {
  printf '%s\n' 'legacy payload delimiter moved or disappeared' >&2; exit 1;
}

env "${common_env[@]}" "$root/ct_exec.sh" --singularity \
  --ct-bootstrap "$bootstrap" -- "$image" command
mapfile -t argv < "$log"
[[ ${argv[0]} == exec ]] || { printf '%s\n' 'singularity bootstrap did not use exec' >&2; exit 1; }

env "${common_env[@]}" "$root/ct_shell.sh" --podman \
  --ct-bootstrap "$bootstrap" -- "$image"
mapfile -t argv < "$log"
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
error=$(env "${common_env[@]}" "$root/ct_exec.sh" --apptainer \
  --ct-env 'UNSAFE=$(date)' "$image" command 2>&1)
status=$?
set -e
[[ $status -ne 0 && $error == *'invalid --ct-env'* ]] || {
  printf '%s\n' 'shell-evaluated environment value was accepted' >&2; exit 1;
}

printf '%s\n' 'container-tools bootstrap tests passed'
