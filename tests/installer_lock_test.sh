#!/usr/bin/env bash
# Prove all installers for one managed prefix serialize independently of recovery storage.
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
work=/tmp/mkchad-v1/container-tools-c11/installer-lock-test
version=1.0.0
architecture=$(uname -m | tr '[:upper:]' '[:lower:]')
libc=glibc
source_commit=0000000000000000000000000000000000000000
identity="$version-$architecture-$libc-$source_commit"
archive_root="container-tools-$identity"
rm -rf -- "$work"
mkdir -p -- "$work/scripts" "$work/archive/$archive_root/bin" \
  "$work/archive/$archive_root/share/container-tools" "$work/prefix/.container-tools"
cp -- "$root/scripts/install-package.sh" "$work/scripts/"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$work/scripts/verify-package.sh"
chmod 700 -- "$work/scripts/verify-package.sh"
printf '#!/usr/bin/env bash\nprintf '\''{"source_commit":"%s"}\\n'\''\n' \
  "$source_commit" > "$work/archive/$archive_root/bin/container-tools"
chmod 700 -- "$work/archive/$archive_root/bin/container-tools"
for name in ct_exec.sh ct_shell.sh ct_instance_exec.sh ct_mount_detector.sh ct_args.sh; do
  printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$work/archive/$archive_root/bin/$name"
  chmod 700 -- "$work/archive/$archive_root/bin/$name"
done
tar -C "$work/archive" -czf "$work/package.tar.gz" "$archive_root"
digest=$(sha256sum "$work/package.tar.gz" | awk '{print $1}')
args=(--archive "$work/package.tar.gz" --sha256 "$digest" --version "$version" \
  --source-commit "$source_commit" --architecture "$architecture" --libc "$libc" \
  --prefix "$work/prefix" --recovery-dir "$work/recovery-two")

flock "$work/prefix/.container-tools/container-tools-install.lock" \
  bash -c "touch \"$work/lock-ready\"; sleep 5" &
holder=$!
while [[ ! -f $work/lock-ready ]]; do sleep 0.01; done
set +e
timeout 1 bash "$work/scripts/install-package.sh" --apply "${args[@]}"
status=$?
set -e
kill "$holder" 2>/dev/null || true
wait "$holder" 2>/dev/null || true
[[ $status == 124 && ! -e $work/prefix/bin/container-tools ]] || {
  printf '%s\n' 'installer did not block on the managed-prefix lock' >&2
  exit 1
}
bash "$work/scripts/install-package.sh" --apply "${args[@]}"
[[ -x $work/prefix/bin/container-tools ]] || {
  printf '%s\n' 'installer did not complete after the managed-prefix lock was released' >&2
  exit 1
}
printf '%s\n' 'container-tools installer lock test passed'
