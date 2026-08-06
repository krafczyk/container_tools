#!/usr/bin/env bash
set -euo pipefail

work=${1:?pass /tmp/mkchad-v1/host-root-projection work directory}
root=${2:?pass the container_tools checkout}
root=$(realpath "$root")
work=$(realpath -m -- "$work")
[[ $work == /tmp/mkchad-v1/host-root-projection/* ]] || {
  printf '%s\n' 'test directory must be beneath /tmp/mkchad-v1/host-root-projection' >&2
  exit 2
}
[[ ! -e $work ]] || {
  printf '%s\n' 'test directory already exists' >&2
  exit 2
}

mkdir -p "$work/root" "$work/cache"
# shellcheck disable=SC1091
. "$root/ct_library.sh"

fixture_root="$work/root"
mkdir -p "$fixture_root"
mkdir -p "$fixture_root/usr" "$fixture_root/var" "$fixture_root/run" "$fixture_root/data"
mkdir -p "$fixture_root/space dir" "$fixture_root"/$'tab\tdir' "$fixture_root"/$'slash\\dir'
mkdir -p "$fixture_root/proc" "$fixture_root/sys" "$fixture_root/dev"
ln -s usr "$fixture_root/bin"

mountinfo="$work/mountinfo"
cat > "$mountinfo" <<EOF
24 1 8:1 / / rw,relatime - ext4 /dev/root rw
25 24 8:2 / $fixture_root/usr rw,relatime - ext4 /dev/sda rw
26 24 8:3 / $fixture_root/var rw,relatime - nfs server:/var rw
27 24 8:4 / $fixture_root/run rw,relatime - tmpfs run rw
28 24 8:5 / $fixture_root/data rw,relatime - lustre fs rw
29 24 8:6 / $fixture_root/space\\040dir rw,relatime - gpfs fs rw
30 24 8:7 / $fixture_root/tab\\011dir rw,relatime - ext4 /dev/sdb rw
31 24 8:8 / $fixture_root/slash\\134dir rw,relatime - ext4 /dev/sdc rw
32 24 0:1 / $fixture_root/proc rw,relatime - proc proc rw
33 24 0:2 / $fixture_root/sys rw,relatime - sysfs sysfs rw
34 24 0:3 / $fixture_root/dev rw,relatime - devtmpfs devtmpfs rw
EOF

CT_HOST_PROJECTION_MOUNTINFO="$mountinfo"
CT_HOST_PROJECTION_SOURCE_ROOT="$fixture_root"
CT_HOST_PROJECTION_CACHE_ROOT="$work/cache"
CT_HOST_PROJECTION_BOOT_ID=boot-a
CT_HOST_PROJECTION_HOSTNAME='host-a'
CT_HOST_PROJECTION_EXECUTABLE=/usr/bin/docker
CT_HOST_PROJECTION_ENDPOINT=local

assert_contains() {
  local expected=$1 item
  for item in "${CT_HOST_PROJECTION_SOURCES[@]}"; do
    [[ $item == "$expected" ]] && return 0
  done
  printf 'missing projection source: %q\n' "$expected" >&2
  exit 1
}

assert_not_contains() {
  local unwanted=$1 item
  for item in "${CT_HOST_PROJECTION_SOURCES[@]}"; do
    [[ $item != "$unwanted" ]] || {
      printf 'unexpected projection source: %q\n' "$unwanted" >&2
      exit 1
    }
  done
}

ct_host_projection_build_direct
[[ ${CT_HOST_PROJECTION_SOURCES[0]} == "$fixture_root" ]]
assert_contains "$fixture_root/usr"
assert_contains "$fixture_root/var"
assert_contains "$fixture_root/run"
assert_contains "$fixture_root/data"
assert_contains "$fixture_root/space dir"
assert_contains "$fixture_root"/$'tab\tdir'
assert_contains "$fixture_root/slash\\dir"
assert_not_contains "$fixture_root/proc"
assert_not_contains "$fixture_root/sys"
assert_not_contains "$fixture_root/dev"
[[ ${CT_HOST_PROJECTION_DESTINATIONS[0]} == "/host$fixture_root" ]]
previous=
for source in "${CT_HOST_PROJECTION_SOURCES[@]}"; do
  [[ -z $previous || $previous < $source ]] || {
    printf '%s\n' 'direct projection order is not lexical and deterministic' >&2
    exit 1
  }
  previous=$source
done
ct_host_projection_profile_digest
profile_a=$CT_HOST_PROJECTION_PROFILE_DIGEST
cp "$mountinfo" "$work/mountinfo-volatile"
printf '99 1 99:99 / %s rw,relatime - ext4 /dev/volatile rw\n' "$fixture_root" >> "$work/mountinfo-volatile"
CT_HOST_PROJECTION_MOUNTINFO="$work/mountinfo-volatile"
ct_host_projection_build_direct
ct_host_projection_profile_digest
[[ $profile_a == "$CT_HOST_PROJECTION_PROFILE_DIGEST" ]] || {
  printf '%s\n' 'volatile mount presentation changed the generated profile' >&2
  exit 1
}
CT_HOST_PROJECTION_MOUNTINFO="$mountinfo"
ct_host_projection_render_mounts docker
[[ ${CT_HOST_PROJECTION_MOUNT_ARGS[*]} == *'bind-recursive=disabled'* ]]
ct_host_projection_render_mounts podman
[[ ${CT_HOST_PROJECTION_MOUNT_ARGS[*]} == *'bind-nonrecursive'* ]]

ct_host_projection_build_fallback
fallback_count=${#CT_HOST_PROJECTION_SOURCES[@]}
[[ ${CT_HOST_PROJECTION_SOURCES[0]} == "$fixture_root/bin" || ${CT_HOST_PROJECTION_SOURCES[0]} == "$fixture_root/data" ]]
assert_not_contains "$fixture_root/proc"
assert_not_contains "$fixture_root/sys"
assert_not_contains "$fixture_root/dev"
previous=
for source in "${CT_HOST_PROJECTION_SOURCES[@]}"; do
  [[ -z $previous || $previous < $source ]] || {
    printf '%s\n' 'fallback projection order is not lexical and deterministic' >&2
    exit 1
  }
  previous=$source
done

ct_host_projection_selection_key docker image:one option-a numeric-supplementary
key_a=$CT_HOST_PROJECTION_SELECTION_KEY
ct_host_projection_selection_key docker image:one option-a numeric-supplementary
[[ $key_a == "$CT_HOST_PROJECTION_SELECTION_KEY" ]]
CT_HOST_PROJECTION_BOOT_ID=boot-b
ct_host_projection_selection_key docker image:one option-a numeric-supplementary
[[ $key_a != "$CT_HOST_PROJECTION_SELECTION_KEY" ]]
CT_HOST_PROJECTION_BOOT_ID=boot-a
ct_host_projection_selection_key docker image:two option-a numeric-supplementary
[[ $key_a != "$CT_HOST_PROJECTION_SELECTION_KEY" ]]
ct_host_projection_selection_key docker image:one option-b numeric-supplementary
[[ $key_a != "$CT_HOST_PROJECTION_SELECTION_KEY" ]]
ct_host_projection_selection_key docker image:one option-a numeric-supplementary
[[ $key_a == "$CT_HOST_PROJECTION_SELECTION_KEY" ]]

ct_host_projection_cache_write "$key_a" fallback complete numeric-supplementary proven
ct_host_projection_cache_read "$key_a"
[[ $CT_HOST_PROJECTION_STRATEGY == fallback ]]
[[ ${#CT_HOST_PROJECTION_SOURCES[@]} -eq $fallback_count ]]

# A warm fallback record validates only stored entries and does not rebuild the
# root plan. A retargeted or missing selected source is omitted rather than
# rediscovered.
rm -- "$fixture_root/bin"
ln -s data "$fixture_root/bin"
ct_host_projection_cache_read "$key_a"
ct_host_projection_validate_fallback auto
assert_not_contains "$fixture_root/bin"
rm -- "$fixture_root/bin"
ln -s usr "$fixture_root/bin"
rm -rf -- "${fixture_root:?}/var"
ct_host_projection_cache_read "$key_a"
ct_host_projection_validate_fallback auto
[[ $CT_HOST_PROJECTION_COMPLETE == partial ]]
assert_not_contains "$fixture_root/var"
ct_host_projection_cache_read "$key_a"
if ct_host_projection_validate_fallback required; then
  printf '%s\n' 'required mode accepted a missing cached fallback source' >&2
  exit 1
fi

# The stored fallback plan is validated as one aggregate operation. A hung
# resolver cannot multiply its deadline by the number of stored candidates.
slow_bin="$work/slow-bin"
mkdir "$slow_bin"
cat > "$slow_bin/realpath" <<'EOF'
#!/usr/bin/env bash
sleep 2
EOF
chmod 755 "$slow_bin/realpath"
ct_host_projection_cache_read "$key_a"
if PATH="$slow_bin:$PATH" CT_HOST_PROJECTION_PLAN_TIMEOUT=0.1 ct_host_projection_validate_fallback auto; then
  printf '%s\n' 'hung aggregate fallback validation succeeded' >&2
  exit 1
fi

# A tampered recursive-root fallback cannot bypass kernel API exclusions during
# warm validation, and its destination is never accepted from record text.
tampered_key=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
CT_HOST_PROJECTION_SOURCES=("$fixture_root")
CT_HOST_PROJECTION_TARGETS=("$(realpath "$fixture_root")")
ct_host_projection_cache_write "$tampered_key" fallback complete primary-only tampered
ct_host_projection_cache_read "$tampered_key"
ct_host_projection_validate_fallback auto
[[ ${#CT_HOST_PROJECTION_SOURCES[@]} == 0 && $CT_HOST_PROJECTION_COMPLETE == partial ]]

# Corrupt, future, and oversized records are cache misses. Cache file mode is
# deliberately not a validity input.
printf 'future\0' > "$work/cache/$key_a"
if ct_host_projection_cache_read "$key_a"; then
  printf '%s\n' 'future cache record was accepted' >&2
  exit 1
fi
ct_host_projection_cache_write "$key_a" none complete primary-only unavailable
chmod 666 "$work/cache/$key_a"
ct_host_projection_cache_read "$key_a"
[[ $CT_HOST_PROJECTION_STRATEGY == none ]]
truncate -s 1048577 "$work/cache/$key_a"
if ct_host_projection_cache_read "$key_a"; then
  printf '%s\n' 'oversized cache record was accepted' >&2
  exit 1
fi

# Cache publication admits the exact documented record boundaries and rejects
# the first over-limit pair without parsing a raw topology snapshot.
boundary_key=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
segment=$(printf '%*s' 125 '')
segment=${segment// /x}
CT_HOST_PROJECTION_SOURCES=()
CT_HOST_PROJECTION_TARGETS=()
for ((pair = 0; pair < 4096; pair++)); do
  CT_HOST_PROJECTION_SOURCES+=("/$segment")
  CT_HOST_PROJECTION_TARGETS+=("/$segment")
done
ct_host_projection_cache_write "$boundary_key" fallback complete primary-only boundary
boundary_size=$(stat -c '%s' -- "$work/cache/$boundary_key")
padding=$((1048576 - boundary_size))
(( padding >= 0 )) || {
  printf '%s\n' 'boundary fixture unexpectedly exceeded the record limit' >&2
  exit 1
}
extra=$(printf '%*s' "$padding" '')
extra=${extra// /z}
CT_HOST_PROJECTION_TARGETS[4095]+=$extra
ct_host_projection_cache_write "$boundary_key" fallback complete primary-only boundary
[[ $(stat -c '%s' -- "$work/cache/$boundary_key") == 1048576 ]] || {
  printf '%s\n' 'exact-size cache record was not accepted' >&2
  exit 1
}
ct_host_projection_cache_read "$boundary_key"
[[ ${#CT_HOST_PROJECTION_SOURCES[@]} == 4096 ]]
CT_HOST_PROJECTION_SOURCES+=(/overflow)
CT_HOST_PROJECTION_TARGETS+=(/overflow)
if ct_host_projection_cache_write "$boundary_key" fallback complete primary-only boundary; then
  printf '%s\n' 'over-limit fallback pair count was accepted' >&2
  exit 1
fi

# Refresh starts a cold selection even with a valid record. A failed selector
# does not replace the previously published result.
refresh_key=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee
CT_HOST_PROJECTION_SOURCES=()
CT_HOST_PROJECTION_TARGETS=()
ct_host_projection_cache_write "$refresh_key" none complete primary-only unavailable
failed_selector() { return 1; }
if ct_host_projection_select "$refresh_key" 1 failed_selector; then
  printf '%s\n' 'failed refresh was accepted' >&2
  exit 1
fi
ct_host_projection_cache_read "$refresh_key"
[[ $CT_HOST_PROJECTION_STRATEGY == none ]]

# Two cache misses for one key serialize one cold publication. The second
# caller rereads after locking and consumes the result without running proof.
concurrent_key=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd
publication_log="$work/publications"
worker="$work/select-worker.sh"
cat > "$worker" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
. "$1/ct_library.sh"
CT_HOST_PROJECTION_CACHE_ROOT=$2
publication_log=$3
selection_key=$4
selector() {
  printf '%s\n' "$$" >> "$publication_log"
  sleep 0.2
  CT_HOST_PROJECTION_SOURCES=()
  CT_HOST_PROJECTION_TARGETS=()
  CT_HOST_PROJECTION_STRATEGY=none
  CT_HOST_PROJECTION_COMPLETE=complete
  CT_HOST_PROJECTION_GROUP_MODE=primary-only
  CT_HOST_PROJECTION_REASON=unavailable
}
ct_host_projection_select "$selection_key" 0 selector
EOF
chmod 755 "$worker"
"$worker" "$root" "$work/cache" "$publication_log" "$concurrent_key" &
worker_one=$!
"$worker" "$root" "$work/cache" "$publication_log" "$concurrent_key" &
worker_two=$!
wait "$worker_one"
wait "$worker_two"
[[ $(wc -l < "$publication_log") == 1 ]] || {
  printf '%s\n' 'concurrent selection published more than once' >&2
  exit 1
}

# A held selection lock fails within its caller-specified bounded wait.
ct_host_projection_acquire_lock "$refresh_key"
if CT_HOST_PROJECTION_LOCK_TIMEOUT=0.1 bash -c '
  set -euo pipefail
  . "$1/ct_library.sh"
  CT_HOST_PROJECTION_CACHE_ROOT=$2
  ct_host_projection_acquire_lock "$3"
' bash "$root" "$work/cache" "$refresh_key"; then
  printf '%s\n' 'held projection lock was acquired by a second caller' >&2
  exit 1
fi
ct_host_projection_release_lock

# Interrupting a waiter releases its inherited descriptor; a later caller can
# acquire the same key rather than inheriting a stuck cold-path lock.
ct_host_projection_acquire_lock "$refresh_key"
bash -c '
  set -euo pipefail
  . "$1/ct_library.sh"
  CT_HOST_PROJECTION_CACHE_ROOT=$2
  ct_host_projection_acquire_lock "$3"
' bash "$root" "$work/cache" "$refresh_key" &
interrupted_waiter=$!
sleep 0.1
kill -TERM "$interrupted_waiter"
if wait "$interrupted_waiter"; then
  printf '%s\n' 'interrupted projection lock waiter exited successfully' >&2
  exit 1
fi
ct_host_projection_release_lock
CT_HOST_PROJECTION_LOCK_TIMEOUT=0.1 ct_host_projection_acquire_lock "$refresh_key"
ct_host_projection_release_lock

printf '%s\n' 'container-tools host projection tests passed'
