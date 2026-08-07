#!/usr/bin/env bash
# Run neutral U1 vectors against the current Bash implementation or a later
# installed native package without carrying the Bash implementation as fixture data.
set -euo pipefail

readonly fixture_schema='schema	container-tools.bash-baseline-fixtures/v1'
readonly fixture_root_name='bash-baseline'

usage() {
  printf '%s\n' 'usage: parity_test.sh --generate-fixtures DIRECTORY'
  printf '%s\n' '       parity_test.sh --verify-fixtures'
  printf '%s\n' '       parity_test.sh --implementation bash|native --root DIRECTORY --work /tmp/mkchad-v1/container-tools-c11/RUN_ID'
}

script_dir=$(dirname "$(realpath "$0")")
checked_in_fixtures="$script_dir/fixtures/$fixture_root_name"

write_contracts() {
  local destination=$1
  mkdir -p -- "$destination"
  printf '%s\n' "$fixture_schema" \
    $'mount-arguments\tct_args.sh\targv\tpass\twhitespace-tokenized-config-and-delimiter-preservation' \
    $'dry-exec\tct_exec.sh\tdry-run\tpass\tno-projection-or-manifest-state' \
    $'remote-exec\tct_exec.sh\tremote-launch\tpass\tno-local-manifest' \
    $'bootstrap\tct_exec.sh,ct_shell.sh\targv-and-status\tpass\tall-four-outer-runtime-shapes' \
    $'persistent\tct_instance_exec.sh\tstate-and-cleanup\tpass\tcreate-reuse-mismatch-concurrency-interruption' \
    $'projection\tct_exec.sh,ct_shell.sh\tmanifest-and-diagnostics\tpass\tdirect-fallback-none-and-source-mutation' \
    $'runtime-config\tct_library.sh\tconfig-and-buildx-cache\tpass\tsuccess-failure-and-private-storage' \
    > "$destination/contracts.tsv"
  printf '%s\n' '--exclude-path /vector path --add-path /literal;delimiter' > "$destination/mount.conf"
  printf '%s\n' "$fixture_schema" \
    $'mount-arguments\tpass\targv' \
    $'dry-exec\tpass\tdry-run' \
    $'remote-exec\tpass\tremote-launch' > "$destination/expected-results.tsv"
}

implementation=
root=
work=
mode=
while [[ $# -gt 0 ]]; do
  case "$1" in
    --generate-fixtures)
      [[ $# == 2 && -z $mode ]] || { usage >&2; exit 2; }
      mode=generate
      work=$2
      shift 2
      ;;
    --verify-fixtures)
      [[ $# == 1 && -z $mode ]] || { usage >&2; exit 2; }
      mode=verify
      shift
      ;;
    --implementation)
      [[ $# -ge 2 && -z $implementation ]] || { usage >&2; exit 2; }
      implementation=$2
      shift 2
      ;;
    --root)
      [[ $# -ge 2 && -z $root ]] || { usage >&2; exit 2; }
      root=$2
      shift 2
      ;;
    --work)
      [[ $# -ge 2 && -z $work ]] || { usage >&2; exit 2; }
      work=$2
      shift 2
      ;;
    *) usage >&2; exit 2 ;;
  esac
done

if [[ $mode == generate ]]; then
  [[ ! -e $work ]] || { printf '%s\n' 'fixture destination already exists' >&2; exit 2; }
  write_contracts "$work"
  exit 0
fi

if [[ $mode == verify ]]; then
  verify_work=$(mktemp -d /tmp/mkchad-v1/container-tools-c11/parity-fixtures.XXXXXX)
  readonly fixture_files=(contracts.tsv mount.conf expected-results.tsv)
  trap 'rm -rf -- "$verify_work"' EXIT
  write_contracts "$verify_work/$fixture_root_name"
  for fixture_file in "${fixture_files[@]}"; do
    cmp -- "$checked_in_fixtures/$fixture_file" "$verify_work/$fixture_root_name/$fixture_file"
  done
  exit 0
fi

[[ -n $implementation && -n $root && -n $work && $work == /tmp/mkchad-v1/container-tools-c11/* && ! -e $work ]] || {
  usage >&2
  exit 2
}
root=$(realpath "$root")
mkdir -p -- "$work/home" "$work/xdg-config" "$work/xdg-state" "$work/runtime" "$work/fake-bin"
chmod 700 -- "$work/home" "$work/xdg-config" "$work/xdg-state" "$work/runtime"

case "$implementation" in
  bash)
    [[ -x $root/ct_exec.sh && -x $root/ct_args.sh && -f $root/ct_library.sh ]] || { usage >&2; exit 2; }
    ;;
  native)
    # Installed compatibility entry points exercise the same neutral vectors
    # after U8 without preserving the private Bash implementation boundary.
    [[ -x $root/container-tools && -x $root/ct_exec.sh && -x $root/ct_args.sh ]] || {
      printf '%s\n' 'native parity implementation unavailable' >&2
      exit 77
    }
    ;;
  *) usage >&2; exit 2 ;;
esac

common_env=(HOME="$work/home" XDG_CONFIG_HOME="$work/xdg-config" XDG_STATE_HOME="$work/xdg-state" XDG_RUNTIME_DIR="$work/runtime" PATH="$work/fake-bin:$PATH" CT_MOUNT_CFG="$work/missing-mount.conf" CT_HOST_PROJECTION_CACHE_ROOT="$work/projection-cache" CT_MOUNT_PLAN_STATE_ROOT="$work/mount-plans")
cp -- "$checked_in_fixtures/mount.conf" "$work/mount.conf"
mount_args=$(env "${common_env[@]}" "$root/ct_args.sh" "$work/mount.conf" --exclude-path /extra path --add-path /with,comma)
[[ $mount_args == '--exclude-path /vector path /extra path --add-path /literal;delimiter /with,comma' ]] || {
  printf '%s\n' 'mount argument baseline changed' >&2
  exit 1
}

source="$work/source with spaces"
mkdir -- "$source"
dry_output=$(env "${common_env[@]}" CT_DRY_RUN=1 "$root/ct_exec.sh" --docker --ct-bind "$source:/container path" --ct-env 'VECTOR=value with spaces' -- image:local command 'argument;delimiter')
[[ $dry_output == *'VECTOR=value\ with\ spaces'* && $dry_output == *'argument\;delimiter'* \
  && ! -e $work/projection-cache && ! -e $work/mount-plans ]] || {
  printf '%s\n' 'dry execution baseline changed or created state' >&2
  exit 1
}

cat > "$work/fake-bin/docker" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$CT_PARITY_BACKEND_LOG"
EOF
chmod 755 "$work/fake-bin/docker"
env "${common_env[@]}" CT_PARITY_BACKEND_LOG="$work/backend.log" DOCKER_HOST=tcp://remote.invalid \
  "$root/ct_exec.sh" --docker image:local command
[[ -f $work/backend.log && $(<"$work/backend.log") == *'run'* && $(<"$work/backend.log") != *'.container-tools-mount-plan'* \
  && ! -e $work/mount-plans ]] || {
  printf '%s\n' 'remote execution baseline created a local manifest' >&2
  exit 1
}

printf '%s\n' "$fixture_schema" \
  $'mount-arguments\tpass\targv' \
  $'dry-exec\tpass\tdry-run' \
  $'remote-exec\tpass\tremote-launch' > "$work/results.tsv"
cmp -- "$checked_in_fixtures/expected-results.tsv" "$work/results.tsv"
printf '%s\n' 'container-tools Bash parity vectors passed'
