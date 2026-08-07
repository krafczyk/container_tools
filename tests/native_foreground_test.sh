#!/usr/bin/env bash
# Exercise the installed native foreground interface against argv-capturing runtimes.
set -euo pipefail

work=${1:?pass a fresh task-specific work directory}
tool=${2:?pass an installed container-tools executable}
[[ $work == /tmp/mkchad-v1/container-tools-c11/* && ! -e $work ]] || exit 2
mkdir -p "$work/fake" "$work/home" "$work/source"
printf '1 0 0:1 / / rw - ext4 root rw\n' > "$work/mountinfo"
log="$work/backend.log"
cat > "$work/fake/docker" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$CT_NATIVE_RUNTIME_LOG"
if [[ $1 == container && $2 == inspect ]]; then
  printf '%s\n' "${!#}"
fi
exit "${CT_NATIVE_RUNTIME_STATUS:-0}"
EOF
chmod 755 "$work/fake/docker"

common=(HOME="$work/home" PATH="$work/fake:$PATH" XDG_STATE_HOME="$work/state"
  CT_HOST_PROJECTION_CACHE_ROOT="$work/cache" CT_HOST_PROJECTION_SOURCE_ROOT="$work/source"
  CT_HOST_PROJECTION_MOUNTINFO="$work/mountinfo" DOCKER_CONTEXT=default CT_NATIVE_RUNTIME_LOG="$log")
env "${common[@]}" "$tool" exec --docker --ct-bind "$work/source:/workspace" image command
mapfile -t argv < "$log"
manifest=''
for ((index = 0; index < ${#argv[@]}; ++index)); do
  [[ ${argv[index]} == --mount ]] || continue
  descriptor=${argv[index + 1]}
  if [[ $descriptor == type=bind,source=*,target=/.container-tools-mount-plan,readonly ]]; then
    manifest=${descriptor#type=bind,source=}
    manifest=${manifest%,target=/.container-tools-mount-plan,readonly}
  fi
done
[[ -f $manifest && ! -L $manifest ]] || exit 1
mapfile -d '' -t fields < "$manifest"
[[ ${fields[0]} == ct-mount-plan-v1 && ${fields[2]} == docker && ${fields[6]} =~ ^[1-9][0-9]*$ ]] || exit 1
digest=$({ printf '%s\0' "${fields[0]}"; printf '%s\0' "${fields[@]:2}"; } | sha256sum)
[[ ${digest%% *} == "${fields[1]}" ]] || exit 1

set +e
env "${common[@]}" CT_NATIVE_RUNTIME_STATUS=37 "$tool" exec --docker image command
status=$?
set -e
[[ $status == 37 ]] || exit 1

env "${common[@]}" CT_DRY_RUN=1 CT_MOUNT_PLAN_STATE_ROOT="$work/dry-state" "$tool" exec --docker image command
[[ ! -e "$work/dry-state" ]] || exit 1
env "${common[@]}" XDG_STATE_HOME="$work/remote-state" DOCKER_HOST=tcp://remote.invalid "$tool" exec --docker image command
[[ ! -e "$work/remote-state" ]] || exit 1
