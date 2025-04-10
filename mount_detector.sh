#!/bin/bash
# This script detects major mount points and prints them.
# It is designed to work in both Bash and zsh.
#
# The root mount ("/") is always excluded.
# By default, additional directories such as /run and /var are also excluded.
#
# Users may extend the filters via command line:
#   --exclude-fs fs1 fs2 ...
#   --exclude-path path1 path2 ...
#   --add-path path1 path2 ...
#
# For help, run: ./detect_mounts.sh --help

# -------------------------------
# Default Exclusion Settings
# -------------------------------

# Define a space‐separated list of filesystem types to exclude.
exclude_fs="proc sysfs tmpfs devtmpfs devpts securityfs cgroup pstore efivarfs debugfs tracefs configfs fusectl cgroup2 mqueue hugetlbfs"

# Define an array of directory paths to exclude (besides "/").
exclude_paths=("/run" "/var" "/sys" "/proc")

# Arrays to hold user-specified additional filters.
extra_exclude_fs=()
extra_exclude_paths=()
explicit_paths=()

# -------------------------------
# Parse Command Line Arguments
# -------------------------------

while [[ $# -gt 0 ]]; do
  case "$1" in
    --exclude-fs)
      shift
      # Process all following parameters that do not start with '--'
      while [[ $# -gt 0 && "$1" != --* ]]; do
        extra_exclude_fs+=("$1")
        shift
      done
      ;;
    --exclude-path)
      shift
      while [[ $# -gt 0 && "$1" != --* ]]; do
        extra_exclude_paths+=("$1")
        shift
      done
      ;;
    --add-path)
      shift
      while [[ $# -gt 0 && "$1" != --* ]]; do
        explicit_paths+=("$1")
        shift
      done
      ;;
    --help|-h)
      echo "Usage: $0 [--exclude-fs fs1 fs2 ...] [--exclude-path path1 path2 ...] [--add-path path1 path2 ...]"
      echo ""
      echo "  --exclude-fs      Add filesystem types to exclude (besides the defaults)."
      echo "  --exclude-path    Add mount path prefixes to exclude (besides the defaults)."
      echo "  --add-path        Explicitly add paths to the final list (after filtering detected mounts)."
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      echo "Use --help for usage information."
      exit 1
      ;;
  esac
done

# Merge user-specified additional filters into the default lists.
if [ ${#extra_exclude_fs[@]} -gt 0 ]; then
  exclude_fs="$exclude_fs ${extra_exclude_fs[*]}"
fi

if [ ${#extra_exclude_paths[@]} -gt 0 ]; then
  # Append additional paths to the array.
  exclude_paths+=( "${extra_exclude_paths[@]}" )
fi

# -------------------------------
# Helper Functions
# -------------------------------

# Check if a given filesystem type is in the exclusion list.
is_excluded_fs() {
  local fs="$1"
  for ex in $exclude_fs; do
    if [ "$fs" = "$ex" ]; then
      return 0  # Filesystem type is excluded.
    fi
  done
  return 1  # Not excluded.
}

# Check if a mount point path should be excluded based on the exclude_paths array.
# Returns true if the path exactly matches or is nested under one of the exclude paths.
is_excluded_path() {
  local path="$1"
  for ex in "${exclude_paths[@]}"; do
    if [[ "$path" == "$ex" || "$path" == "$ex"/* ]]; then
      return 0  # Path is to be excluded.
    fi
  done
  return 1  # Path is not excluded.
}

# -------------------------------
# Main Processing
# -------------------------------

# Array to hold candidate mount points.
mount_points=()

# Read /proc/mounts line by line.
# Fields in /proc/mounts: device mount_point fs_type options dump pass
while read -r device mount_point fs_type options dump pass; do
  # Always skip the root mount.
  if [ "$mount_point" = "/" ]; then
    continue
  fi

  # Skip if the filesystem type is in the exclusion list.
  if is_excluded_fs "$fs_type"; then
    continue
  fi

  # Only process mount points that begin with '/'.
  case "$mount_point" in
    /*) ;;  # OK.
    *) continue ;;
  esac

  # Skip mount points that fall under additional excluded paths.
  if is_excluded_path "$mount_point"; then
    continue
  fi

  mount_points+=("$mount_point")
done < /proc/mounts

# Remove duplicate mount points.
unique_mounts=()
for mp in "${mount_points[@]}"; do
  skip=
  for ump in "${unique_mounts[@]}"; do
    if [ "$mp" = "$ump" ]; then
      skip=1
      break
    fi
  done
  [ -n "$skip" ] || unique_mounts+=("$mp")
done

# Sort the mounts by the length of their path (shortest first).
sorted_mounts=( $(for mp in "${unique_mounts[@]}"; do
    printf "%s\t%s\n" "${#mp}" "$mp"
  done | sort -n | cut -f2) )

# Remove nested mount points (keeping only the top-level ones).
final_mounts=()
for m in "${sorted_mounts[@]}"; do
  nested=0
  for fm in "${final_mounts[@]}"; do
    case "$m" in
      "$fm"|"$fm"/*) nested=1 ;;
    esac
    if [ $nested -eq 1 ]; then
      break
    fi
  done
  if [ $nested -eq 0 ]; then
    final_mounts+=("$m")
  fi
done

# -------------------------------
# Append Explicitly Specified Paths
# -------------------------------
# Allow the user to explicitly add paths. These additions are appended
# to the final list if they don't already exist.
for ep in "${explicit_paths[@]}"; do
  duplicate=0
  for mp in "${final_mounts[@]}"; do
    if [ "$ep" = "$mp" ]; then
      duplicate=1
      break
    fi
  done
  if [ $duplicate -eq 0 ]; then
    final_mounts+=("$ep")
  fi
done

# -------------------------------
# Output the Results
# -------------------------------
for mp in "${final_mounts[@]}"; do
  echo "$mp"
done

# Optional: Generate bind mount options for a container engine (e.g., Docker)
# Uncomment the following block to display Docker bind mount options.
#: <<'END_OPTIONAL'
#docker_opts=()
#for mp in "${final_mounts[@]}"; do
#  docker_opts+=("-v" "${mp}:${mp}")
#done
#echo
#echo "Docker run options:"
#echo "${docker_opts[@]}"
#END_OPTIONAL
