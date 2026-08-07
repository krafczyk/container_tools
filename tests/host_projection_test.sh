#!/usr/bin/env bash
# shellcheck disable=SC2034 # The sourced library consumes fixture globals dynamically.
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
mkdir -p "$fixture_root/usr" "$fixture_root/var" "$fixture_root/run/netns" "$fixture_root/data"
mkdir -p "$fixture_root/run/service/nested-api" "$fixture_root/run/service/kept"
mkdir -p "$fixture_root/run/docker/netns"
: > "$fixture_root/run/service-file"
chmod 000 "$fixture_root/run/docker"
mkdir -p "$fixture_root/space dir" "$fixture_root"/$'tab\tdir' "$fixture_root"/$'slash\\dir'
mkdir -p "$fixture_root/proc" "$fixture_root/sys" "$fixture_root/dev"
ln -s usr "$fixture_root/bin"
ln -s proc "$fixture_root/kernel-link"
ln -s missing-target "$fixture_root/broken-link"

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
35 27 0:4 / $fixture_root/run/netns rw,relatime - nsfs nsfs rw
36 27 0:5 / $fixture_root/run/service/nested-api rw,relatime - nsfs nsfs rw
37 27 0:6 / $fixture_root/run/docker/netns rw,relatime - nsfs nsfs rw
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
assert_not_contains "$fixture_root/run/netns"
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
direct_profile=$CT_HOST_PROJECTION_PROFILE_DIGEST
CT_HOST_PROJECTION_DESTINATIONS[0]=/host/changed-destination
ct_host_projection_profile_digest
[[ $direct_profile != "$CT_HOST_PROJECTION_PROFILE_DIGEST" ]] || {
  printf '%s\n' 'generated destination did not change the semantic profile' >&2
  exit 1
}
CT_HOST_PROJECTION_DESTINATIONS[0]="/host$fixture_root"
ct_host_projection_profile_digest
[[ $direct_profile == "$CT_HOST_PROJECTION_PROFILE_DIGEST" ]] || {
  printf '%s\n' 'generated profile did not restore from its semantic tuple' >&2
  exit 1
}
CT_HOST_PROJECTION_TARGETS[0]="$fixture_root/alternate-target"
ct_host_projection_profile_digest
[[ $direct_profile != "$CT_HOST_PROJECTION_PROFILE_DIGEST" ]] || {
  printf '%s\n' 'generated resolved target did not change the semantic profile' >&2
  exit 1
}
CT_HOST_PROJECTION_TARGETS[0]="$(realpath "$fixture_root")"
CT_HOST_PROJECTION_STRATEGY=fallback
ct_host_projection_profile_digest
[[ $direct_profile != "$CT_HOST_PROJECTION_PROFILE_DIGEST" ]] || {
  printf '%s\n' 'generated strategy did not change the semantic profile' >&2
  exit 1
}
CT_HOST_PROJECTION_STRATEGY=direct
ct_host_projection_profile_digest apptainer
[[ $direct_profile != "$CT_HOST_PROJECTION_PROFILE_DIGEST" ]] || {
  printf '%s\n' 'generated recursion policy did not change the semantic profile' >&2
  exit 1
}
ct_host_projection_profile_digest docker
[[ $direct_profile == "$CT_HOST_PROJECTION_PROFILE_DIGEST" ]] || {
  printf '%s\n' 'generated profile changed after restoring its backend policy' >&2
  exit 1
}
CT_HOST_PROJECTION_MOUNTINFO="$mountinfo"
ct_host_projection_render_mounts docker
[[ ${CT_HOST_PROJECTION_MOUNT_ARGS[*]} == *'bind-recursive=disabled'* ]]
ct_host_projection_render_mounts podman
[[ ${CT_HOST_PROJECTION_MOUNT_ARGS[*]} == *'bind-nonrecursive'* ]]

ct_host_projection_build_fallback
fallback_count=${#CT_HOST_PROJECTION_SOURCES[@]}
[[ $CT_HOST_PROJECTION_COMPLETE == complete ]] || {
  printf '%s\n' 'bounded fallback decomposition did not preserve complete ordinary content' >&2
  exit 1
}
[[ ${CT_HOST_PROJECTION_SOURCES[0]} == "$fixture_root/bin" || ${CT_HOST_PROJECTION_SOURCES[0]} == "$fixture_root/data" ]]
assert_not_contains "$fixture_root/proc"
assert_not_contains "$fixture_root/sys"
assert_not_contains "$fixture_root/dev"
assert_not_contains "$fixture_root/broken-link"
assert_not_contains "$fixture_root/kernel-link"
assert_not_contains "$fixture_root/run"
assert_not_contains "$fixture_root/run/docker"
assert_not_contains "$fixture_root/run/docker/netns"
assert_not_contains "$fixture_root/run/netns"
assert_not_contains "$fixture_root/run/service"
assert_not_contains "$fixture_root/run/service/nested-api"
assert_contains "$fixture_root/run/service/kept"
assert_contains "$fixture_root/run/service-file"
previous=
for source in "${CT_HOST_PROJECTION_SOURCES[@]}"; do
  [[ -z $previous || $previous < $source ]] || {
    printf '%s\n' 'fallback projection order is not lexical and deterministic' >&2
    exit 1
  }
  previous=$source
done

default_split_depth=${CT_HOST_PROJECTION_FALLBACK_MAX_DEPTH:-64}
CT_HOST_PROJECTION_FALLBACK_MAX_DEPTH=1
ct_host_projection_build_fallback
[[ $CT_HOST_PROJECTION_COMPLETE == partial ]] || {
  printf '%s\n' 'fallback split-depth exhaustion did not remain explicitly partial' >&2
  exit 1
}
assert_not_contains "$fixture_root/run"
assert_not_contains "$fixture_root/run/service"
CT_HOST_PROJECTION_FALLBACK_MAX_DEPTH=$default_split_depth
ct_host_projection_build_fallback

# Top-level kernel API trees are outside the projection contract. Omitting only
# those trees must not make an otherwise complete fallback warn or refuse.
excluded_root="$work/excluded-only-root"
mkdir -p "$excluded_root/usr" "$excluded_root/proc" "$excluded_root/sys" "$excluded_root/dev"
excluded_mountinfo="$work/excluded-only-mountinfo"
cat > "$excluded_mountinfo" <<EOF
24 1 8:1 / / rw,relatime - ext4 /dev/root rw
25 24 8:2 / $excluded_root/usr rw,relatime - ext4 /dev/sda rw
26 24 0:1 / $excluded_root/proc rw,relatime - proc proc rw
27 24 0:2 / $excluded_root/sys rw,relatime - sysfs sysfs rw
28 24 0:3 / $excluded_root/dev rw,relatime - devtmpfs devtmpfs rw
EOF
CT_HOST_PROJECTION_SOURCE_ROOT="$excluded_root"
CT_HOST_PROJECTION_MOUNTINFO="$excluded_mountinfo"
ct_host_projection_build_fallback
[[ $CT_HOST_PROJECTION_COMPLETE == complete && ${#CT_HOST_PROJECTION_SOURCES[@]} -eq 1 \
  && ${CT_HOST_PROJECTION_SOURCES[0]} == "$excluded_root/usr" ]] || {
  printf '%s\n' 'intentional top-level kernel exclusions made fallback projection incomplete' >&2
  exit 1
}
CT_HOST_PROJECTION_SOURCE_ROOT="$fixture_root"
CT_HOST_PROJECTION_MOUNTINFO="$mountinfo"
ct_host_projection_build_fallback

# Enumeration returns only a deterministic bounded prefix and marks omitted
# entries explicitly rather than loading every child into the parent shell.
bounded_root="$work/bounded-root"
mkdir -p "$bounded_root/c" "$bounded_root/a" "$bounded_root/b"
ct_host_projection_list_children "$bounded_root" 2
[[ $CT_HOST_PROJECTION_CHILDREN_COMPLETE == 0 \
  && ${#CT_HOST_PROJECTION_CHILDREN[@]} == 2 \
  && ${CT_HOST_PROJECTION_CHILDREN[0]} == "$bounded_root/a" \
  && ${CT_HOST_PROJECTION_CHILDREN[1]} == "$bounded_root/b" ]] || {
  printf '%s\n' 'fallback child enumeration did not return a bounded lexical prefix' >&2
  exit 1
}
default_record_max_bytes=$CT_HOST_PROJECTION_RECORD_MAX_BYTES
CT_HOST_PROJECTION_RECORD_MAX_BYTES=$((${#bounded_root} + 3))
ct_host_projection_list_children "$bounded_root" 3
[[ $CT_HOST_PROJECTION_CHILDREN_COMPLETE == 0 \
  && ${#CT_HOST_PROJECTION_CHILDREN[@]} == 1 \
  && ${CT_HOST_PROJECTION_CHILDREN[0]} == "$bounded_root/a" ]] || {
  printf '%s\n' 'fallback child enumeration exceeded its byte cap' >&2
  exit 1
}
CT_HOST_PROJECTION_RECORD_MAX_BYTES=$default_record_max_bytes

current_policy_version=$CT_HOST_PROJECTION_POLICY_VERSION
legacy_partial_keys=()
for legacy_policy_version in host-projection-v1 host-projection-v2 host-projection-v3 host-projection-v4; do
  CT_HOST_PROJECTION_POLICY_VERSION=$legacy_policy_version
  ct_host_projection_selection_key docker image:one option-a numeric-supplementary
  legacy_partial_keys+=("$CT_HOST_PROJECTION_SELECTION_KEY")
done
CT_HOST_PROJECTION_POLICY_VERSION=$current_policy_version
ct_host_projection_selection_key docker image:one option-a numeric-supplementary
key_a=$CT_HOST_PROJECTION_SELECTION_KEY
for legacy_partial_key in "${legacy_partial_keys[@]}"; do
  [[ $legacy_partial_key != "$key_a" ]] || {
    printf '%s\n' 'decomposed fallback reused a legacy partial selection record' >&2
    exit 1
  }
done
ct_host_projection_selection_key docker image:one option-a numeric-supplementary
[[ $key_a == "$CT_HOST_PROJECTION_SELECTION_KEY" ]]
MOUNT_ARGS=(--bind "$fixture_root/data:/container/extra")
ct_host_projection_selection_key docker image:one option-a numeric-supplementary
[[ $key_a == "$CT_HOST_PROJECTION_SELECTION_KEY" ]] || {
  printf '%s\n' 'an explicit bind changed the projection selection key' >&2
  exit 1
}
MOUNT_ARGS=()
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

# A previously proven partial record remains partial even when every retained
# path currently validates. Required mode must not promote it to a payload.
partial_key=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
CT_HOST_PROJECTION_SOURCES=("$fixture_root/bin")
CT_HOST_PROJECTION_TARGETS=("$(realpath "$fixture_root/bin")")
ct_host_projection_cache_write "$partial_key" fallback partial primary-only partial
ct_host_projection_cache_read "$partial_key"
ct_host_projection_validate_fallback auto
[[ $CT_HOST_PROJECTION_COMPLETE == partial ]] || {
  printf '%s\n' 'warm validation promoted a cached partial fallback record' >&2
  exit 1
}
ct_host_projection_cache_read "$partial_key"
if ct_host_projection_validate_fallback required; then
  printf '%s\n' 'required mode promoted a cached partial fallback record' >&2
  exit 1
fi

# The stored fallback plan is validated as one aggregate operation. A hung
# resolver cannot multiply its deadline by the number of stored candidates.
slow_bin="$work/slow-bin"
mkdir "$slow_bin"
cat > "$slow_bin/realpath" <<'EOF'
#!/usr/bin/env bash
if [[ ${1:-} == -m ]]; then
  exec /usr/bin/realpath "$@"
fi
sleep 2
EOF
chmod 755 "$slow_bin/realpath"
cat > "$slow_bin/find" <<'EOF'
#!/usr/bin/env bash
sleep 5
EOF
chmod 755 "$slow_bin/find"
planning_started=$SECONDS
CT_HOST_PROJECTION_COLD_DEADLINE=$((SECONDS + 1))
if PATH="$slow_bin:$PATH" ct_host_projection_build_fallback; then
  printf '%s\n' 'hung top-level fallback enumeration succeeded' >&2
  exit 1
fi
unset CT_HOST_PROJECTION_COLD_DEADLINE
(( SECONDS - planning_started < 4 )) || {
  printf '%s\n' 'top-level fallback enumeration exceeded its aggregate deadline' >&2
  exit 1
}

# Sorting and producer status are part of the same bounded enumeration. A hung
# or failed sorter cannot escape the deadline or publish a false complete plan.
slow_sort_bin="$work/slow-sort-bin"
mkdir "$slow_sort_bin"
cat > "$slow_sort_bin/sort" <<'EOF'
#!/usr/bin/env bash
case "$(cat "$(dirname "$0")/mode")" in
  slow-sort)
    printf '%s\n' "$$" > "$(dirname "$0")/sort-pid"
    sleep 5
    ;;
  fail-sort) exit 9 ;;
esac
exec /usr/bin/sort "$@"
EOF
cat > "$slow_sort_bin/find" <<'EOF'
#!/usr/bin/env bash
[[ $(cat "$(dirname "$0")/mode") != fail-find ]] || exit 8
exec /usr/bin/find "$@"
EOF
chmod 755 "$slow_sort_bin/sort"
chmod 755 "$slow_sort_bin/find"
printf '%s\n' slow-sort > "$slow_sort_bin/mode"
planning_started=$SECONDS
CT_HOST_PROJECTION_COLD_DEADLINE=$((SECONDS + 1))
if PATH="$slow_sort_bin:$PATH" ct_host_projection_build_fallback; then
  printf '%s\n' 'hung fallback sort succeeded' >&2
  exit 1
fi
unset CT_HOST_PROJECTION_COLD_DEADLINE
(( SECONDS - planning_started < 4 )) || {
  printf '%s\n' 'fallback sort exceeded its aggregate deadline' >&2
  exit 1
}
slow_sort_pid=$(<"$slow_sort_bin/sort-pid")
if kill -0 "$slow_sort_pid" 2>/dev/null; then
  printf '%s\n' 'timed-out fallback sorter remained alive' >&2
  exit 1
fi
printf '%s\n' fail-sort > "$slow_sort_bin/mode"
if PATH="$slow_sort_bin:$PATH" ct_host_projection_build_fallback; then
  printf '%s\n' 'failed fallback sort produced a selection' >&2
  exit 1
fi
printf '%s\n' fail-find > "$slow_sort_bin/mode"
if PATH="$slow_sort_bin:$PATH" ct_host_projection_build_fallback; then
  printf '%s\n' 'failed fallback enumeration produced a selection' >&2
  exit 1
fi

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

# A lexical fallback source outside an excluded subtree may resolve into one.
# Both the stored and current targets must be rejected before rendering.
target_key=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
rm -- "$fixture_root/bin"
ln -s proc "$fixture_root/bin"
CT_HOST_PROJECTION_SOURCES=("$fixture_root/bin")
CT_HOST_PROJECTION_TARGETS=("$fixture_root/proc")
ct_host_projection_cache_write "$target_key" fallback complete primary-only target-kernel-api
ct_host_projection_cache_read "$target_key"
ct_host_projection_validate_fallback auto
[[ ${#CT_HOST_PROJECTION_SOURCES[@]} == 0 && $CT_HOST_PROJECTION_COMPLETE == partial ]] || {
  printf '%s\n' 'cached fallback target inside a kernel API filesystem was retained' >&2
  exit 1
}
rm -- "$fixture_root/bin"
ln -s usr "$fixture_root/bin"

# A warm lexical alias can remain unchanged while a new excluded mount appears
# beneath its canonical target. Validation must omit that alias rather than
# importing the new kernel-backed subtree through a recursive bind.
target_descendant_key=9999999999999999999999999999999999999999999999999999999999999999
target_descendant="$fixture_root/target-descendant"
target_alias="$fixture_root/target-alias"
mkdir -p "$target_descendant/ordinary" "$target_descendant/kernel-api"
ln -s target-descendant "$target_alias"
CT_HOST_PROJECTION_SOURCES=("$target_alias")
CT_HOST_PROJECTION_TARGETS=("$(realpath "$target_alias")")
ct_host_projection_cache_write "$target_descendant_key" fallback complete primary-only target-descendant
cp "$mountinfo" "$work/mountinfo-target-descendant"
printf '98 24 0:98 / %s rw,relatime - nsfs nsfs rw\n' \
  "$target_descendant/kernel-api" >> "$work/mountinfo-target-descendant"
CT_HOST_PROJECTION_MOUNTINFO="$work/mountinfo-target-descendant"
ct_host_projection_cache_read "$target_descendant_key"
ct_host_projection_validate_fallback auto
[[ ${#CT_HOST_PROJECTION_SOURCES[@]} == 0 && $CT_HOST_PROJECTION_COMPLETE == partial ]] || {
  printf '%s\n' 'cached alias imported a new excluded mount beneath its canonical target' >&2
  exit 1
}
CT_HOST_PROJECTION_MOUNTINFO="$mountinfo"

# Existing mount descriptors must compare source and destination fields exactly,
# rather than accepting destination prefixes.
CT_HOST_PROJECTION_SOURCES=(/host/data)
CT_HOST_PROJECTION_TARGETS=(/host/data)
CT_HOST_PROJECTION_DESTINATIONS=(/host/data)
CT_HOST_PROJECTION_COMPLETE=complete
MOUNT_ARGS=(--mount 'type=bind,source=/host/data,target=/host/data-old,bind-recursive=disabled')
ct_host_projection_resolve_mount_conflicts
[[ ${#CT_HOST_PROJECTION_SOURCES[@]} == 1 && $CT_HOST_PROJECTION_COMPLETE == complete ]] || {
  printf '%s\n' 'mount destination prefix was treated as an equivalent projection bind' >&2
  exit 1
}
MOUNT_ARGS=(--mount 'type=bind,source=/host/data,target=/host/data,bind-recursive=disabled')
ct_host_projection_resolve_mount_conflicts
[[ ${#CT_HOST_PROJECTION_SOURCES[@]} == 0 && $CT_HOST_PROJECTION_COMPLETE == complete ]] || {
  printf '%s\n' 'exact equivalent projection bind was not retained' >&2
  exit 1
}
CT_HOST_PROJECTION_SOURCES=(/)
CT_HOST_PROJECTION_TARGETS=(/)
CT_HOST_PROJECTION_DESTINATIONS=(/host)
CT_HOST_PROJECTION_COMPLETE=complete
MOUNT_ARGS=(--mount 'type=bind,source=/other,target=/host/etc')
ct_host_projection_resolve_mount_conflicts
[[ ${#CT_HOST_PROJECTION_SOURCES[@]} == 0 && $CT_HOST_PROJECTION_COMPLETE == partial ]] || {
  printf '%s\n' 'nested existing host mount did not make the generated root partial' >&2
  exit 1
}
MOUNT_ARGS=()

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

# Foreground launchers default to an automatic projection and normalize Docker
# payloads to an image-first `run` invocation. U1 has no launch integration, so
# this is intentionally the first U2 red proof.
launcher_fake="$work/launcher-fake"
launcher_log="$work/launcher.log"
mkdir -p "$launcher_fake"
cat > "$launcher_fake/docker" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
call_log="$(dirname "$0")/calls"
state="$(dirname "$0")/probe-state"
owner="$(dirname "$0")/probe-owner"
case "${1:-}" in
  create)
    printf '%s\n' probe-create >> "$call_log"
    printf '%s\n' "$@" > "$(dirname "$0")/probe-create-argv"
    [[ -z ${DOCKER_HOST:-}${CONTAINER_HOST:-} ]] || printf 'create:%s:%s\n' "${DOCKER_HOST:-}" "${CONTAINER_HOST:-}" >> "$(dirname "$0")/endpoint-observed"
    [[ -z ${CT_HOST_PROJECTION_LAUNCH_LOG:-} ]] || : > "$(dirname "$0")/payload-env-leaked"
    name=
    for ((index = 1; index < $#; index++)); do
      [[ ${!index} == --name ]] || continue
      next=$((index + 1))
      name=${!next}
      break
    done
    printf '%s\n' "$name" > "$(dirname "$0")/probe-name"
    if [[ -e $(dirname "$0")/force-direct-fail && "$*" == *bind-recursive=disabled* ]]; then
      rm -- "$(dirname "$0")/force-direct-fail"
      exit 20
    fi
    if [[ -e $(dirname "$0")/force-fallback-fail ]]; then
      rm -- "$(dirname "$0")/force-fallback-fail"
      exit 20
    fi
    if [[ -e $(dirname "$0")/force-transient-fail ]]; then
      exit 9
    fi
    if [[ -e $(dirname "$0")/force-create-timeout ]]; then
      : > "$state"
      printf '%s\n' "$name" > "$owner"
      sleep 5
    fi
    if [[ -e $(dirname "$0")/force-create-malformed ]]; then
      : > "$state"
      printf '%s\n' "$name" > "$owner"
      printf '%s\n' 'malformed probe id'
      exit 0
    fi
    group_mode=none
    for argument; do
      [[ $argument == numeric ]] && group_mode=numeric
    done
    if [[ $group_mode == numeric ]]; then
      printf '%s\n' numeric > "$state"
    else
      printf '%s\n' none > "$state"
    fi
    printf '%s\n' "$name" > "$owner"
    printf '%s\n' fake-container-id
    exit 0
    ;;
  start)
    printf '%s\n' probe-start >> "$call_log"
    [[ -z ${DOCKER_HOST:-}${CONTAINER_HOST:-} ]] || printf 'start:%s:%s\n' "${DOCKER_HOST:-}" "${CONTAINER_HOST:-}" >> "$(dirname "$0")/endpoint-observed"
    if [[ -e $(dirname "$0")/force-start-timeout ]]; then
      sleep 5
    fi
    printf 'ct-host-projection-group=%s\n' "$(<"$state")"
    ;;
  rm)
    printf '%s\n' probe-cleanup >> "$call_log"
    [[ -z ${DOCKER_HOST:-}${CONTAINER_HOST:-} ]] || printf 'rm:%s:%s\n' "${DOCKER_HOST:-}" "${CONTAINER_HOST:-}" >> "$(dirname "$0")/endpoint-observed"
    if [[ -e $(dirname "$0")/force-cleanup-fail ]]; then
      exit 9
    fi
    rm -f -- "$state" "$owner"
    ;;
  container)
    [[ ${2:-} == inspect ]] || exit 64
    printf '%s\n' inspect >> "$call_log"
    [[ -f $owner ]] || exit 1
    cat "$owner"
    ;;
  *)
    printf '%s\n' payload >> "$call_log"
    printf '%s\n' "$@" > "$CT_HOST_PROJECTION_LAUNCH_LOG"
    ;;
esac
EOF
chmod 755 "$launcher_fake/docker"
ln -s docker "$launcher_fake/podman"

# The effective endpoint selector is key material and explicit remote selectors
# are rejected before either a warm probe or payload invocation.
CT_HOST_PROJECTION_ENDPOINT=
unset DOCKER_HOST DOCKER_CONTEXT DOCKER_MACHINE_NAME CONTAINER_HOST CONTAINER_CONNECTION PODMAN_CONNECTION
ct_host_projection_selection_key docker image:local option numeric-supplementary
selector_key=$CT_HOST_PROJECTION_SELECTION_KEY
DOCKER_HOST=unix:///tmp/docker-one.sock ct_host_projection_selection_key docker image:local option numeric-supplementary
[[ $selector_key != "$CT_HOST_PROJECTION_SELECTION_KEY" ]] || {
  printf '%s\n' 'Docker Unix endpoint did not change the selection key' >&2
  exit 1
}
DOCKER_CONTEXT=default ct_host_projection_selection_key docker image:local option numeric-supplementary
[[ $selector_key != "$CT_HOST_PROJECTION_SELECTION_KEY" ]] || {
  printf '%s\n' 'explicit Docker default context did not change the selection key' >&2
  exit 1
}
unset DOCKER_HOST DOCKER_CONTEXT
CT_HOST_PROJECTION_ENDPOINT=local
env HOME="$work/home" PATH="$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command
mapfile -t launch_argv < "$launcher_log"
[[ ${launch_argv[0]} == run && ${launch_argv[1]} == --rm ]] || {
  printf '%s\n' 'foreground Docker payload was not normalized to run --rm' >&2
  exit 1
}
[[ " ${launch_argv[*]} " == *' target=/host,'* || " ${launch_argv[*]} " == *'target=/host,'* ]] || {
  printf '%s\n' 'foreground launch omitted the default host projection' >&2
  exit 1
}
mapfile -t launch_calls < "$launcher_fake/calls"
[[ ${launch_calls[*]} == 'probe-create probe-start probe-cleanup payload' ]] || {
  printf '%s\n' 'cold foreground launch did not use one managed probe and one payload' >&2
  exit 1
}
[[ $(<"$launcher_fake/probe-create-argv") == *'target=/.ct-host-projection-locality,readonly'* ]] || {
  printf '%s\n' 'cold Docker probe omitted the client locality nonce bind' >&2
  exit 1
}
cache_records=("$work/launcher-cache"/[0-9a-f]*)
CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-cache"
ct_host_projection_cache_read "$(basename "${cache_records[0]}")"
[[ $CT_HOST_PROJECTION_GROUP_MODE == numeric-supplementary ]] || {
  printf '%s\n' 'conclusive numeric supplementary group mode was not retained' >&2
  exit 1
}
CT_HOST_PROJECTION_CACHE_ROOT="$work/cache"

env HOME="$work/home" PATH="$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command
mapfile -t launch_calls < "$launcher_fake/calls"
[[ ${launch_calls[*]} == 'probe-create probe-start probe-cleanup payload payload' ]] || {
  printf '%s\n' 'warm foreground launch created a runtime probe' >&2
  exit 1
}

# Warm direct realization shares one metadata budget; a hanging resolver cannot
# stall once per mounted descendant or start a new probe.
: > "$launcher_fake/calls"
SECONDS=0
env HOME="$work/home" PATH="$slow_bin:$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_PLAN_TIMEOUT=0.1 CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command
warm_direct_elapsed=$SECONDS
mapfile -t launch_calls < "$launcher_fake/calls"
[[ $warm_direct_elapsed -lt 6 && ${launch_calls[*]} == payload ]] || {
  printf '%s\n' 'warm direct planning exceeded its metadata budget or reprobed' >&2
  exit 1
}

# Foreground dry runs retain the normal payload, environment, and bootstrap
# preview but must not prepare a projection, whether its cache is cold or warm.
dry_bootstrap="$work/dry-bootstrap"
generated_host_mount="target=/host\\,"
printf '%s\n' '#!/usr/bin/env bash' > "$dry_bootstrap"
chmod 755 "$dry_bootstrap"
for dry_launcher in exec shell; do
  for dry_cache_state in cold existing; do
    dry_cache="$work/dry-$dry_launcher-$dry_cache_state-cache"
    dry_cache_snapshot="$work/dry-$dry_launcher-$dry_cache_state-cache-before"
    if [[ $dry_cache_state == existing ]]; then
      cp -a "$work/launcher-cache" "$dry_cache"
      cp -a "$dry_cache" "$dry_cache_snapshot"
    fi
    : > "$launcher_fake/calls"
    if [[ $dry_launcher == exec ]]; then
      dry_output=$(env HOME="$work/home" PATH="$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
        CT_DRY_RUN=1 CT_HOST_PROJECTION_CACHE_ROOT="$dry_cache" \
        "$root/ct_exec.sh" --docker --ct-env DRY_TEST=value --ct-bootstrap "$dry_bootstrap" -- \
        image:local dry-command 'dry argument')
      [[ $dry_output == *"$dry_bootstrap"*'target=/.container-tools-bootstrap'* \
        && $dry_output == *'--env DRY_TEST=value'* \
        && $dry_output == *'image:local /.container-tools-bootstrap dry-command dry\ argument '* ]] || {
        printf 'exec dry-run payload preview changed: %s\n' "$dry_output" >&2
        exit 1
      }
    else
      dry_output=$(env HOME="$work/home" PATH="$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
        CT_DRY_RUN=1 CT_HOST_PROJECTION_CACHE_ROOT="$dry_cache" \
        "$root/ct_shell.sh" --docker --ct-env DRY_TEST=value --ct-bootstrap "$dry_bootstrap" \
        --ct-container-shell /bin/bash -- image:local)
      [[ $dry_output == *"$dry_bootstrap"*'target=/.container-tools-bootstrap'* \
        && $dry_output == *'--env DRY_TEST=value'* \
        && $dry_output == *'image:local /.container-tools-bootstrap /bin/bash -i '* ]] || {
        printf 'shell dry-run payload preview changed: %s\n' "$dry_output" >&2
        exit 1
      }
    fi
    [[ ! -s $launcher_fake/calls && $dry_output != *"$generated_host_mount"* \
      && $dry_output != *'target=/host/'* ]] || {
      printf '%s dry run invoked a backend or rendered a generated host projection: %s\n' \
        "$dry_launcher/$dry_cache_state" "$dry_output" >&2
      exit 1
    }
    if [[ $dry_cache_state == cold ]]; then
      [[ ! -e $dry_cache ]] || {
        printf '%s\n' 'cold foreground dry run created projection state' >&2
        exit 1
      }
    else
      diff -r -- "$dry_cache_snapshot" "$dry_cache" >/dev/null || {
        printf '%s\n' 'warm foreground dry run changed projection state' >&2
        exit 1
      }
    fi
  done
done

# A validated local Unix endpoint is retained for each Docker probe operation,
# while no other payload environment enters the clean probe environment.
: > "$launcher_fake/calls"
rm -f -- "$launcher_fake/endpoint-observed"
env HOME="$work/home" PATH="$launcher_fake:$PATH" DOCKER_HOST=unix:///tmp/docker-probe.sock \
  CT_HOST_PROJECTION_BOOT_ID=endpoint-proof CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-endpoint-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command
mapfile -t endpoint_calls < "$launcher_fake/endpoint-observed"
[[ ${endpoint_calls[*]} == 'create:unix:///tmp/docker-probe.sock: start:unix:///tmp/docker-probe.sock: rm:unix:///tmp/docker-probe.sock:' ]] || {
  printf '%s\n' 'validated Docker endpoint was not carried into every probe operation' >&2
  exit 1
}
[[ ! -e $launcher_fake/payload-env-leaked ]] || {
  printf '%s\n' 'payload environment leaked into a Docker probe' >&2
  exit 1
}

: > "$launcher_fake/calls"
rm -f -- "$launcher_fake/endpoint-observed"
env HOME="$work/home" PATH="$launcher_fake:$PATH" CONTAINER_HOST=unix:///tmp/podman-probe.sock \
  CT_HOST_PROJECTION_BOOT_ID=podman-endpoint-proof CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-podman-endpoint-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --podman image:local command
mapfile -t endpoint_calls < "$launcher_fake/endpoint-observed"
[[ ${endpoint_calls[*]} == 'create::unix:///tmp/podman-probe.sock start::unix:///tmp/podman-probe.sock rm::unix:///tmp/podman-probe.sock' ]] || {
  printf '%s\n' 'validated Podman endpoint was not carried into every probe operation' >&2
  exit 1
}
[[ ! -e $launcher_fake/payload-env-leaked ]] || {
  printf '%s\n' 'payload environment leaked into a Podman probe' >&2
  exit 1
}

# A single aggregate cold deadline covers planning as well as probing. A hanging
# resolver degrades an automatic launch without starting a probe or multiplying
# a timeout per candidate.
: > "$launcher_fake/calls"
set +e
SECONDS=0
env HOME="$work/home" PATH="$slow_bin:$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_BOOT_ID=hanging-planning CT_HOST_PROJECTION_COLD_TIMEOUT=1 \
  CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-hanging-planning-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command \
  >/dev/null 2>&1
hanging_planning_status=$?
elapsed=$SECONDS
set -e
mapfile -t launch_calls < "$launcher_fake/calls"
[[ $hanging_planning_status == 0 && $elapsed -lt 6 && ${launch_calls[*]} == payload ]] || {
  printf '%s\n' 'hanging cold planning exceeded its aggregate deadline or started a probe' >&2
  exit 1
}

# A conclusive direct rejection performs one conservative fallback aggregate,
# cleans it exactly, and still dispatches the payload only once.
touch "$launcher_fake/force-direct-fail"
: > "$launcher_fake/calls"
env HOME="$work/home" PATH="$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_BOOT_ID=fallback-proof CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-fallback-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command
mapfile -t launch_calls < "$launcher_fake/calls"
[[ ${launch_calls[*]} == 'probe-create probe-create probe-start probe-cleanup payload' ]] || {
  printf '%s\n' 'fallback selection did not use exactly one direct and one managed fallback probe' >&2
  exit 1
}
rm -f -- "$launcher_fake/force-direct-fail"

# A timeout with confirmed cleanup is unavailable rather than terminal, so auto
# dispatches exactly one original payload without generated projection.
: > "$launcher_fake/calls"
touch "$launcher_fake/force-start-timeout"
env HOME="$work/home" PATH="$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_BOOT_ID=start-timeout CT_HOST_PROJECTION_COLD_TIMEOUT=3 \
  CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-start-timeout-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command
rm -- "$launcher_fake/force-start-timeout"
mapfile -t launch_calls < "$launcher_fake/calls"
[[ ${launch_calls[*]} == 'probe-create probe-start probe-cleanup payload' ]] || {
  printf '%s\n' 'confirmed probe timeout did not dispatch one original payload' >&2
  exit 1
}

# A create can take effect before its client times out or returns malformed
# output. A name-based cleanup is allowed only after the generated ownership
# label is read back; confirmed cleanup permits one automatic payload.
for uncertain_case in create-timeout create-malformed; do
  : > "$launcher_fake/calls"
  touch "$launcher_fake/force-$uncertain_case"
  env HOME="$work/home" PATH="$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
    CT_HOST_PROJECTION_BOOT_ID="$uncertain_case" CT_HOST_PROJECTION_COLD_TIMEOUT=3 \
    CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-$uncertain_case-cache" \
    CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command
  rm -- "$launcher_fake/force-$uncertain_case"
  mapfile -t launch_calls < "$launcher_fake/calls"
  [[ ${launch_calls[*]} == 'probe-create inspect probe-cleanup payload' ]] || {
    printf 'uncertain probe create was not cleaned before one payload: %s\n' "$uncertain_case" >&2
    exit 1
  }
done

# Only typed conclusive rejections may publish `none`. A transient create whose
# ownership cannot be established is terminal and never removes by name.
: > "$launcher_fake/calls"
rm -f -- "$launcher_log"
touch "$launcher_fake/force-transient-fail"
transient_status=0
env HOME="$work/home" PATH="$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_BOOT_ID=transient-failure CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-transient-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command \
  >/dev/null 2>&1 || transient_status=$?
mapfile -t launch_calls < "$launcher_fake/calls"
transient_records=("$work/launcher-transient-cache"/[0-9a-f]*)
[[ ${transient_status:-0} != 0 && ${launch_calls[*]} == 'probe-create inspect' \
  && ! -e ${transient_records[0]} && ! -e $launcher_log ]] || {
  printf '%s\n' 'unowned transient probe dispatched or removed a name collision' >&2
  exit 1
}
rm -- "$launcher_fake/force-transient-fail"

: > "$launcher_fake/calls"
touch "$launcher_fake/force-direct-fail" "$launcher_fake/force-fallback-fail"
env HOME="$work/home" PATH="$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_BOOT_ID=conclusive-none CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-none-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command
mapfile -t launch_calls < "$launcher_fake/calls"
none_records=("$work/launcher-none-cache"/[0-9a-f]*)
[[ ${launch_calls[*]} == 'probe-create probe-create payload' && -e ${none_records[0]} ]] || {
  printf '%s\n' 'conclusive fallback rejection did not publish one reusable none record' >&2
  exit 1
}

# Unconfirmed cleanup is terminal and cannot reach a payload, even in automatic
# mode.
: > "$launcher_fake/calls"
touch "$launcher_fake/force-cleanup-fail"
set +e
env HOME="$work/home" PATH="$launcher_fake:$PATH" CT_MOUNT_CFG="$work/missing-mount-config" \
  CT_HOST_PROJECTION_BOOT_ID=cleanup-fail CT_HOST_PROJECTION_COLD_TIMEOUT=3 \
  CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-cleanup-fail-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker image:local command \
  >/dev/null 2>&1
terminal_status=$?
set -e
rm -- "$launcher_fake/force-cleanup-fail"
mapfile -t launch_calls < "$launcher_fake/calls"
[[ $terminal_status != 0 && " ${launch_calls[*]} " != *' payload '* \
  && " ${launch_calls[*]} " == *' probe-cleanup '* ]] || {
  printf '%s\n' 'terminal probe cleanup failure dispatched or skipped cleanup' >&2
  exit 1
}
rm -f -- "$launcher_log"
set +e
env HOME="$work/home" PATH="$launcher_fake:$PATH" DOCKER_HOST=tcp://remote.invalid \
  CT_MOUNT_CFG="$work/missing-mount-config" CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-cache" \
  CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker --ct-host-root required image:local command >/dev/null 2>&1
required_remote_status=$?
set -e
[[ $required_remote_status != 0 && ! -e $launcher_log ]] || {
  printf '%s\n' 'required remote projection dispatched a payload' >&2
  exit 1
}
for selector_case in docker-context podman-connection; do
  : > "$launcher_fake/calls"
  rm -f -- "$launcher_log"
  set +e
  case "$selector_case" in
    docker-context)
      env HOME="$work/home" PATH="$launcher_fake:$PATH" DOCKER_CONTEXT=remote \
        CT_MOUNT_CFG="$work/missing-mount-config" CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-context-cache" \
        CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --docker --ct-host-root required image:local command >/dev/null 2>&1
      ;;
    podman-connection)
      env HOME="$work/home" PATH="$launcher_fake:$PATH" CONTAINER_CONNECTION=remote \
        CT_MOUNT_CFG="$work/missing-mount-config" CT_HOST_PROJECTION_CACHE_ROOT="$work/launcher-connection-cache" \
        CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" --podman --ct-host-root required image:local command >/dev/null 2>&1
      ;;
  esac
  selector_status=$?
  set -e
  mapfile -t launch_calls < "$launcher_fake/calls"
  [[ $selector_status != 0 && ! -e $launcher_log && ${#launch_calls[@]} == 0 ]] || {
    printf 'explicit remote selector invoked the runtime: %s\n' "$selector_case" >&2
    exit 1
  }
done

# Persisted client selectors are part of the effective endpoint even when no
# selector environment variable is exported.
mkdir -p "$work/home/.docker" "$work/home/.config/containers"
printf '%s\n' '{"currentContext":"remote-build-host"}' > "$work/home/.docker/config.json"
printf '%s\n' '{"Connection":{"Default":"remote-podman"}}' > "$work/home/.config/containers/podman-connections.json"
for persisted_backend in docker podman; do
  : > "$launcher_fake/calls"
  rm -f -- "$launcher_log"
  set +e
  env -u DOCKER_CONTEXT -u DOCKER_HOST -u DOCKER_MACHINE_NAME -u DOCKER_CONFIG -u CONTAINER_CONNECTION \
    -u PODMAN_CONNECTION -u CONTAINER_HOST -u XDG_CONFIG_HOME HOME="$work/home" PATH="$launcher_fake:$PATH" \
    CT_MOUNT_CFG="$work/missing-mount-config" CT_HOST_PROJECTION_CACHE_ROOT="$work/persisted-$persisted_backend-cache" \
    CT_HOST_PROJECTION_LAUNCH_LOG="$launcher_log" "$root/ct_exec.sh" "--$persisted_backend" \
    --ct-host-root required image:local command >/dev/null 2>&1
  persisted_status=$?
  set -e
  [[ $persisted_status != 0 && ! -s $launcher_fake/calls && ! -e $launcher_log ]] || {
    printf 'persisted remote %s selector invoked the runtime\n' "$persisted_backend" >&2
    exit 1
  }
done
rm -f -- "$work/home/.docker/config.json" "$work/home/.config/containers/podman-connections.json"

# The host evidence runner is safe to invoke by default: help and invalid CLI
# produce no report, while a valid disabled invocation emits the closed schema
# and records no runtime claim.
runner="$root/tests/host_projection_runtime_test.sh"
bash "$runner" --help >/dev/null
if bash "$runner" --backend docker --image image:local --work "$work/not-an-approved-runtime-work" >/dev/null 2>&1; then
  printf '%s\n' 'runtime runner accepted an unsafe work root' >&2
  exit 1
fi
runtime_work="/tmp/mkchad-v1/host-root-projection-host/disabled-$$"
[[ ! -e $runtime_work ]] || { printf '%s\n' 'runtime runner fixture already exists' >&2; exit 1; }
set +e
runtime_report=$(PATH="/usr/bin:/bin" bash "$runner" --backend docker --image image:local --work "$runtime_work")
runtime_status=$?
set -e
[[ $runtime_status == 77 && $runtime_report == *'"schema":"container-tools.host-projection-runtime/v1"'* \
  && $runtime_report == *'"id":"HP-HOST-006","status":"skip","reason":"backend-inapplicable"'* \
  && -f "$runtime_work/report.json" ]] || {
  printf '%s\n' 'disabled runtime runner did not emit its unavailable report' >&2
  exit 1
}

# The enabled runner is exercised only through a fake local client. Its passed
# report validates the evidence envelope, not Docker support. A missing warm
# result must make the matrix fail instead of inheriting success from the cold
# or first warm launch.
runtime_fake="$work/runtime-fake"
mkdir "$runtime_fake"
cat > "$runtime_fake/docker" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
write_requested_marker() {
  local script= target= index target_index
  for ((index = 1; index <= $#; index++)); do
    [[ ${!index} != -ec ]] || { target_index=$((index + 1)); script=${!target_index:-}; break; }
  done
  target=${script#*"host-projection-ok > '/host"}
  target=${target%%"'"*}
  [[ $target == "${MKCHAD_TEST_RUNTIME_RESULTS}/"* ]] || exit 74
  [[ ${target##*/} == "${MKCHAD_TEST_SKIP_RESULT:-}" ]] || : > "$target"
}
case "${1:-}" in
  --version) printf '%s\n' 'fake-docker 1.0' ;;
  image)
    [[ ${2:-} == inspect ]] || exit 64
    if [[ -n ${MKCHAD_TEST_IMAGE_ID_FILE:-} && -f $MKCHAD_TEST_IMAGE_ID_FILE ]]; then
      cat "$MKCHAD_TEST_IMAGE_ID_FILE"
    else
      printf '%s\n' 'sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'
    fi
    ;;
  create) printf '%s\n' fake-probe ;;
  start) printf '%s\n' 'ct-host-projection-group=numeric' ;;
  rm) : ;;
  run)
    count=0
    [[ ! -f $MKCHAD_TEST_RUNTIME_COUNT ]] || read -r count < "$MKCHAD_TEST_RUNTIME_COUNT"
    count=$((count + 1))
    printf '%s\n' "$count" > "$MKCHAD_TEST_RUNTIME_COUNT"
    [[ -z ${MKCHAD_TEST_FORGE_OPERATIONS:-} ]] || printf '%s\n' 'probe-start:fallback' >> "$MKCHAD_TEST_FORGE_OPERATIONS"
    write_requested_marker "$@"
    if [[ -n ${MKCHAD_TEST_IMAGE_ID_FILE:-} && ${MKCHAD_TEST_MUTATE_IMAGE:-} == 1 && $count == 1 ]]; then
      printf '%s\n' 'sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff' > "$MKCHAD_TEST_IMAGE_ID_FILE"
    fi
    if [[ -n ${MKCHAD_TEST_MUTATE_SOURCE:-} && $count == 1 ]]; then
      printf '%s\n' '# fake-runtime source mutation' >> "$MKCHAD_TEST_MUTATE_SOURCE"
    fi
    if [[ -n ${MKCHAD_TEST_MUTATE_ADAPTER:-} && $count == 1 ]]; then
      printf '%s\n' '# fake-runtime adapter mutation' >> "$MKCHAD_TEST_MUTATE_ADAPTER"
    fi
    ;;
  *) exit 66 ;;
esac
EOF
chmod 755 "$runtime_fake/docker"

runtime_unlabeled_work="/tmp/mkchad-v1/host-root-projection-host/fake-unlabeled-$$"
set +e
runtime_report=$(PATH="$runtime_fake:$PATH" CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  MKCHAD_TEST_RUNTIME_COUNT="$work/unlabeled-count" MKCHAD_TEST_RUNTIME_RESULTS="$runtime_unlabeled_work/results" \
  bash "$runner" --backend docker --image image:local --work "$runtime_unlabeled_work")
runtime_status=$?
set -e
[[ $runtime_status == 77 && $runtime_report == *'"overall":"unavailable"'* \
  && $runtime_report == *'"reason":"fixture-runtime-unlabeled"'* ]] || {
  printf '%s\n' 'unlabeled deterministic runtime produced host evidence' >&2
  exit 1
}

runtime_enabled_work="/tmp/mkchad-v1/host-root-projection-host/fake-enabled-$$"
mkdir -p "${runtime_enabled_work%/*}"
set +e
runtime_report=$(PATH="$runtime_fake:$PATH" CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  CT_HOST_PROJECTION_RUNTIME_FIXTURE=1 \
  MKCHAD_TEST_FORGE_OPERATIONS="$runtime_enabled_work/operations.log" \
  MKCHAD_TEST_RUNTIME_COUNT="$work/runtime-count" MKCHAD_TEST_RUNTIME_RESULTS="$runtime_enabled_work/results" \
  bash "$runner" --backend docker --image image:local --work "$runtime_enabled_work")
runtime_status=$?
set -e
[[ $runtime_status == 0 && $runtime_report == *'"overall":"passed"'* \
  && $runtime_report == *'"samples":20'* && $runtime_report == *'"strategy_condition":"selected-direct"'* \
  && -s $runtime_enabled_work/operations.log ]] || {
  printf '%s\n' 'fake runtime did not satisfy the complete measured evidence matrix' >&2
  exit 1
}
if bash "$runner" --validate-report "$runtime_enabled_work/report.json"; then
  printf '%s\n' 'normal validator accepted deterministic fixture evidence' >&2
  exit 1
fi
bash "$runner" --validate-fixture-report "$runtime_enabled_work/report.json" || {
  printf '%s\n' 'fixture validator rejected its exact-source report' >&2
  exit 1
}
empty_evidence_report="$work/empty-evidence-report.json"
jq '(.cases[] | select(.id != "HP-HOST-006") | .operations) = {
  inspect:0, probe_create:0, probe_start:0, probe_cleanup:0,
  instance_liveness:0, instance_start:0, instance_stop:0, payload:0,
  probe_tags:{direct:0, fallback:0}
} | (.cases[] | select(.id == "HP-HOST-004") | .phase_operations[]) = {
  inspect:0, probe_create:0, probe_start:0, probe_cleanup:0,
  instance_liveness:0, instance_start:0, instance_stop:0, payload:0,
  probe_tags:{direct:0, fallback:0}
}' "$runtime_enabled_work/report.json" > "$empty_evidence_report"
if bash "$runner" --validate-fixture-report "$empty_evidence_report"; then
  printf '%s\n' 'validator accepted a passed report with empty operation evidence' >&2
  exit 1
fi
concatenated_report="$work/concatenated-report.json"
printf '%s\n%s\n' "$runtime_report" "$runtime_report" > "$concatenated_report"
set +e
bash "$runner" --validate-fixture-report "$concatenated_report"
concatenated_status=$?
set -e
[[ $concatenated_status == 2 ]] || {
  printf '%s\n' 'validator accepted multiple top-level report objects' >&2
  exit 1
}

runtime_abort_work="/tmp/mkchad-v1/host-root-projection-host/fake-abort-$$"
set +e
runtime_report=$(PATH="$runtime_fake:$PATH" CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  CT_HOST_PROJECTION_RUNTIME_FIXTURE=1 CT_HOST_PROJECTION_RUNTIME_ABORT_AFTER_COLD=1 \
  MKCHAD_TEST_RUNTIME_COUNT="$work/abort-count" MKCHAD_TEST_RUNTIME_RESULTS="$runtime_abort_work/results" \
  bash "$runner" --backend docker --image image:local --work "$runtime_abort_work")
runtime_status=$?
set -e
[[ $runtime_status == 1 && $runtime_report == *'"overall":"failed"'* \
  && $runtime_report == *'"reason":"unexpected-exit"'* && -f $runtime_abort_work/report.json ]] || {
  printf '%s\n' 'unexpected runner exit did not emit a failed report' >&2
  exit 1
}

runtime_setup_abort_work="/tmp/mkchad-v1/host-root-projection-host/fake-setup-abort-$$"
set +e
runtime_report=$(PATH="$runtime_fake:$PATH" CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  CT_HOST_PROJECTION_RUNTIME_FIXTURE=1 CT_HOST_PROJECTION_RUNTIME_ABORT_DURING_SETUP=1 \
  MKCHAD_TEST_RUNTIME_COUNT="$work/setup-abort-count" MKCHAD_TEST_RUNTIME_RESULTS="$runtime_setup_abort_work/results" \
  bash "$runner" --backend docker --image image:local --work "$runtime_setup_abort_work")
runtime_status=$?
set -e
[[ $runtime_status == 1 && $runtime_report == *'"overall":"failed"'* \
  && $runtime_report == *'"reason":"unexpected-exit"'* && -f $runtime_setup_abort_work/report.json ]] || {
  printf '%s\n' 'unexpected setup exit did not emit a failed report' >&2
  exit 1
}

runtime_forced_work="/tmp/mkchad-v1/host-root-projection-host/fake-forced-$$"
set +e
runtime_report=$(PATH="$runtime_fake:$PATH" CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  CT_HOST_PROJECTION_RUNTIME_FIXTURE=1 \
  MKCHAD_TEST_RUNTIME_COUNT="$work/forced-count" MKCHAD_TEST_RUNTIME_RESULTS="$runtime_forced_work/results" \
  bash "$runner" --backend docker --force-fallback --image image:local --work "$runtime_forced_work")
runtime_status=$?
set -e
[[ $runtime_status == 0 && $runtime_report == *'"strategy_condition":"forced-fallback"'* \
  && $runtime_report == *'"fallback":'* ]] || {
  printf '%s\n' 'forced fallback was not separately traced and labeled' >&2
  exit 1
}

# A native fake accepts only the staged SIF basename during foreground probing
# and payload execution. This is a staging/schema test, not SingularityCE
# evidence; the report is deliberately retained only as a deterministic fixture.
native_fake="$work/native-runtime-fake"
mkdir "$native_fake"
cat > "$native_fake/singularity" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
write_requested_marker() {
  local script= target= index target_index
  for ((index = 1; index <= $#; index++)); do
    [[ ${!index} != -ec ]] || { target_index=$((index + 1)); script=${!target_index:-}; break; }
  done
  target=${script#*"host-projection-ok > '/host"}
  target=${target%%"'"*}
  [[ $target == "${MKCHAD_TEST_RUNTIME_RESULTS}/"* ]] || exit 74
  : > "$target"
}
case "${1:-}" in
  --version) printf '%s\n' 'fake-singularity 1.0' ;;
  instance)
    case "${2:-}" in
      start)
        args=("$@")
        name=${!#}
        [[ $name =~ ^mkchad-[0-9a-f]{32}$ && ! -e $MKCHAD_TEST_RUNTIME_INSTANCES/$name ]] || exit 75
        identity_source=
        for ((index = 0; index < ${#args[@]} - 1; index++)); do
          if [[ ${args[index]} == --bind && ${args[index + 1]} == *:/.container-tools-instance-identity:ro ]]; then
            identity_source=${args[index + 1]%:/.container-tools-instance-identity:ro}
            break
          fi
        done
        mapfile -t identity_fields 2>/dev/null < "$identity_source" || exit 76
        [[ ${#identity_fields[@]} == 3 && ${identity_fields[0]} == "$name" \
          && ${identity_fields[1]} =~ ^[0-9a-f]{64}$ && ${identity_fields[2]} =~ ^[0-9a-f]{32}$ ]] || exit 76
        : > "$MKCHAD_TEST_RUNTIME_INSTANCES/$name"
        printf '%s\n' "${identity_fields[1]}" > "$MKCHAD_TEST_RUNTIME_INSTANCES/$name.profile"
        printf '%s\n' "${identity_fields[2]}" > "$MKCHAD_TEST_RUNTIME_INSTANCES/$name.nonce"
        ;;
      stop)
        name=${3:-}
        [[ -e $MKCHAD_TEST_RUNTIME_INSTANCES/$name ]] || exit 77
        rm -f -- "$MKCHAD_TEST_RUNTIME_INSTANCES/$name" "$MKCHAD_TEST_RUNTIME_INSTANCES/$name.profile" "$MKCHAD_TEST_RUNTIME_INSTANCES/$name.nonce"
        ;;
      list)
        [[ ${3:-} == --json ]] || exit 78
        name=${4:-}
        if [[ -n $name && -e $MKCHAD_TEST_RUNTIME_INSTANCES/$name ]]; then
          printf '{"instances":[{"instance":"%s"}]}\n' "$name"
        else
          printf '{"instances":[]}\n'
        fi
        ;;
      *) exit 78 ;;
    esac
    ;;
  exec)
    if [[ " $* " == *' instance://'* ]]; then
      name=
      for argument; do [[ $argument != instance://* ]] || { name=${argument#instance://}; break; }; done
      [[ -n $name && -e $MKCHAD_TEST_RUNTIME_INSTANCES/$name ]] || exit 79
      if [[ " $* " == *' -c '* ]]; then
        [[ "$*" == *'/.container-tools-instance-identity'* ]] || exit 80
        expected_profile=${@: -2:1}
        expected_nonce=${!#}
        [[ $expected_profile == "$(<"$MKCHAD_TEST_RUNTIME_INSTANCES/$name.profile")" \
          && ( -z $expected_nonce || $expected_nonce == "$(<"$MKCHAD_TEST_RUNTIME_INSTANCES/$name.nonce")" ) ]] || exit 80
      else
        write_requested_marker "$@"
      fi
    elif [[ " $* " == *'ct-host-projection-group=native'* ]]; then
      [[ " $* " == *'/image.sif'* ]] || exit 71
      printf '%s\n' 'ct-host-projection-group=native'
    else
      [[ " $* " == *'/image.sif'* ]] || exit 72
      write_requested_marker "$@"
    fi
    ;;
  *) exit 73 ;;
esac
EOF
chmod 755 "$native_fake/singularity"
native_image="$work/local-image.sif"
: > "$native_image"
runtime_native_work="/tmp/mkchad-v1/host-root-projection-host/fake-native-$$"
set +e
runtime_report=$(PATH="$native_fake:$PATH" CT_HOST_PROJECTION_RUNTIME_TEST=1 \
  CT_HOST_PROJECTION_RUNTIME_FIXTURE=1 \
  MKCHAD_TEST_RUNTIME_RESULTS="$runtime_native_work/results" \
  bash "$runner" --backend singularity --image "$native_image" --work "$runtime_native_work")
runtime_status=$?
set -e
[[ $runtime_status == 0 && $runtime_report == *'"backend":"singularity"'* \
  && $runtime_report == *'"id":"HP-HOST-006","status":"pass"'* ]] || {
  printf '%s\n' 'staged native SIF or persistent evidence matrix failed' >&2
  exit 1
}

foreign_report="$work/foreign-report.json"
source_commit=$(git -C "$root" rev-parse HEAD)
foreign_payload=${runtime_report/\"source_commit\":\"$source_commit\"/\"source_commit\":\"foreign\"}
printf '%s\n' "$foreign_payload" > "$foreign_report"
if bash "$runner" --validate-report "$foreign_report"; then
  printf '%s\n' 'runner accepted a foreign-source report' >&2
  exit 1
fi
printf '%s\n' '{"schema":"container-tools.host-projection-runtime/v1"}' > "$work/malformed-report.json"
set +e
bash "$runner" --validate-report "$work/malformed-report.json"
malformed_status=$?
set -e
if [[ $malformed_status != 2 ]]; then
  printf '%s\n' 'runner accepted a malformed report' >&2
  exit 1
fi
set +e
bash "$runner" --validate-report "$runtime_work/report.json"
unavailable_status=$?
set -e
[[ $unavailable_status == 77 ]] || {
  printf '%s\n' 'normal validator did not preserve unavailable status' >&2
  exit 1
}

runtime_missing_work="/tmp/mkchad-v1/host-root-projection-host/fake-missing-$$"
set +e
PATH="$runtime_fake:$PATH" CT_HOST_PROJECTION_RUNTIME_TEST=1 CT_HOST_PROJECTION_RUNTIME_FIXTURE=1 MKCHAD_TEST_SKIP_RESULT=warm-20 \
  MKCHAD_TEST_RUNTIME_COUNT="$work/runtime-missing-count" MKCHAD_TEST_RUNTIME_RESULTS="$runtime_missing_work/results" \
  bash "$runner" --backend docker --image image:local --work "$runtime_missing_work" > "$work/missing-runtime-report.json"
runtime_status=$?
set -e
[[ $runtime_status == 1 && $(<"$work/missing-runtime-report.json") == *'"overall":"failed"'* ]] || {
  printf '%s\n' 'missing warm measurement produced a passing runtime report' >&2
  exit 1
}

runtime_mutation_work="/tmp/mkchad-v1/host-root-projection-host/fake-image-mutation-$$"
image_id_file="$work/image-id"
set +e
PATH="$runtime_fake:$PATH" CT_HOST_PROJECTION_RUNTIME_TEST=1 CT_HOST_PROJECTION_RUNTIME_FIXTURE=1 MKCHAD_TEST_MUTATE_IMAGE=1 \
  MKCHAD_TEST_IMAGE_ID_FILE="$image_id_file" MKCHAD_TEST_RUNTIME_COUNT="$work/mutation-count" \
  MKCHAD_TEST_RUNTIME_RESULTS="$runtime_mutation_work/results" \
  bash "$runner" --backend docker --image image:local --work "$runtime_mutation_work" > "$work/mutation-runtime-report.json"
runtime_status=$?
set -e
[[ $runtime_status == 1 && $(<"$work/mutation-runtime-report.json") == *'"overall":"failed"'* ]] || {
  printf '%s\n' 'post-admission image mutation did not fail the runtime report' >&2
  exit 1
}

# A payload can recreate the old projected adapter pathname, but later host
# dispatches continue through the unlinked descriptor rather than that file.
runtime_adapter_mutation_work="/tmp/mkchad-v1/host-root-projection-host/fake-adapter-mutation-$$"
set +e
PATH="$runtime_fake:$PATH" CT_HOST_PROJECTION_RUNTIME_TEST=1 CT_HOST_PROJECTION_RUNTIME_FIXTURE=1 \
  MKCHAD_TEST_MUTATE_ADAPTER="$runtime_adapter_mutation_work/adapter/docker" \
  MKCHAD_TEST_RUNTIME_COUNT="$work/adapter-mutation-count" \
  MKCHAD_TEST_RUNTIME_RESULTS="$runtime_adapter_mutation_work/results" \
  bash "$runner" --backend docker --image image:local --work "$runtime_adapter_mutation_work" > "$work/adapter-mutation-runtime-report.json"
runtime_status=$?
set -e
[[ $runtime_status == 0 && $(<"$work/adapter-mutation-runtime-report.json") == *'"overall":"passed"'* \
  && -s $runtime_adapter_mutation_work/adapter/docker && $(<"$work/adapter-mutation-count") -gt 1 ]] || {
  printf '%s\n' 'projected adapter pathname influenced a later host dispatch' >&2
  exit 1
}

# Copying the closed source set lets the fake mutate an exact launch file
# without touching this checkout; mixed source bytes must fail the report.
source_copy="$work/source-copy"
mkdir -p "$source_copy/tests"
cp "$root"/ct_args.sh "$root"/ct_mount_detector.sh "$root"/ct_library.sh "$root"/ct_exec.sh \
  "$root"/ct_shell.sh "$root"/ct_instance_exec.sh "$source_copy"
cp "$root"/tests/bootstrap_test.sh "$root"/tests/instance_exec_test.sh "$root"/tests/host_projection_test.sh \
  "$root"/tests/host_projection_runtime_test.sh "$source_copy/tests"
runtime_source_mutation_work="/tmp/mkchad-v1/host-root-projection-host/fake-source-mutation-$$"
set +e
PATH="$runtime_fake:$PATH" CT_HOST_PROJECTION_RUNTIME_TEST=1 CT_HOST_PROJECTION_RUNTIME_FIXTURE=1 \
  MKCHAD_TEST_MUTATE_SOURCE="$source_copy/ct_exec.sh" MKCHAD_TEST_RUNTIME_COUNT="$work/source-mutation-count" \
  MKCHAD_TEST_RUNTIME_RESULTS="$runtime_source_mutation_work/results" \
  bash "$source_copy/tests/host_projection_runtime_test.sh" --backend docker --image image:local \
  --work "$runtime_source_mutation_work" > "$work/source-mutation-runtime-report.json"
runtime_status=$?
set -e
[[ $runtime_status == 1 && $(<"$work/source-mutation-runtime-report.json") == *'"overall":"failed"'* \
  && $(<"$work/source-mutation-runtime-report.json") == *'"reason":"source-changed"'* ]] || {
  printf '%s\n' 'post-dispatch source mutation did not fail the runtime report' >&2
  exit 1
}

printf '%s\n' 'container-tools host projection tests passed'
