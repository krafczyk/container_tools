#!/usr/bin/env bash
# Fixture-only native host-projection integration. It never invokes a real runtime.
set -euo pipefail

work=${1:?pass a fresh task-specific work directory}
tool=${2:?pass an installed container-tools executable}
[[ $work == /tmp/mkchad-v1/container-tools-c11/* && ! -e $work ]] || exit 2
mkdir -p "$work/fake" "$work/root/data" "$work/state"
printf '1 0 0:1 / / rw - ext4 root rw\n' > "$work/mountinfo"

for backend in docker podman singularity apptainer; do
  cat > "$work/fake/$backend" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" >> "$CT_NATIVE_PROJECTION_LOG"
if [[ $1 == container && $2 == inspect ]]; then
  printf '%s\n' "${!#}"
fi
EOF
  chmod 755 "$work/fake/$backend"
done

common=(HOME="$work/home" PATH="$work/fake:$PATH" XDG_STATE_HOME="$work/state"
  CT_HOST_PROJECTION_SOURCE_ROOT="$work/root" CT_HOST_PROJECTION_MOUNTINFO="$work/mountinfo"
  CT_HOST_PROJECTION_CACHE_ROOT="$work/cache" DOCKER_HOST=unix:///fixture DOCKER_CONTEXT=default
  DOCKER_MACHINE_NAME= CONTAINER_HOST=unix:///fixture CONTAINER_CONNECTION= PODMAN_CONNECTION=)

for backend in docker podman singularity apptainer; do
  option="--$backend"
  log="$work/$backend.log"
  env "${common[@]}" CT_NATIVE_PROJECTION_LOG="$log" \
    "$tool" exec "$option" --ct-host-root required image command
  grep -q '/host' "$log"
  before=$(wc -l < "$log")
  env "${common[@]}" CT_NATIVE_PROJECTION_LOG="$log" \
    "$tool" exec "$option" --ct-host-root auto image command
  [[ $(wc -l < "$log") -gt $before ]]
  env "${common[@]}" CT_NATIVE_PROJECTION_LOG="$log" \
    "$tool" exec "$option" --ct-host-root required --ct-host-root-refresh image command
  before=$(wc -l < "$log")
  env "${common[@]}" CT_NATIVE_PROJECTION_LOG="$log" CT_DRY_RUN=1 \
    "$tool" exec "$option" --ct-host-root auto image command >/dev/null
  [[ $(wc -l < "$log") == "$before" ]]
done

remote_state="$work/remote-state"
env "${common[@]}" XDG_STATE_HOME="$remote_state" DOCKER_HOST=tcp://remote.invalid \
  CT_NATIVE_PROJECTION_LOG="$work/remote.log" "$tool" exec --docker --ct-host-root auto image command
[[ ! -e $remote_state ]]
