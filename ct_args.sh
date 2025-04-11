#!/bin/bash
# combine_args.sh
# This script combines arguments from a file and the command line.
# The first argument is the file containing arguments (e.g., "--arg1 a b c --arg2 d e f").
# The remaining arguments become additional args (e.g., "--arg1 A B C --arg2 D --arg3 F").
# The script outputs a combined argument string such as:
#   --arg1 a b c A B C --arg2 d e f D --arg3 F

# Ensure we have at least one argument.
if [ "$#" -lt 1 ]; then
  echo "Usage: $0 <file> [arguments...]"
  exit 1
fi

# The first argument is the file.
arg_file="$1"
if [ ! -f "$arg_file" ]; then
  echo "Error: File '$arg_file' not found."
  exit 1
fi

# Read file arguments into an array.
# This splits the file contents on whitespace.
file_args=( $(<"$arg_file") )

# Shift off the file argument; remaining arguments form our extra args.
shift
cmd_args=( "$@" )

# Combine both sources into one array.
combined=( "${file_args[@]}" "${cmd_args[@]}" )

# Use an associative array to group arguments for each flag.
declare -A opts
# And an array to preserve the order in which flags are first seen.
order=()

current=""

# Process each token in the combined list.
for token in "${combined[@]}"; do
  # If token starts with '--', it's a flag.
  if [[ "$token" == --* ]]; then
    current="$token"
    # Record the flag if not already seen.
    if [[ -z "${opts[$current]}" ]]; then
      order+=( "$current" )
      opts["$current"]=""
    fi
  else
    # Otherwise, append the argument to the most recent flag.
    if [[ -n "$current" ]]; then
      if [[ -n "${opts[$current]}" ]]; then
        opts["$current"]="${opts[$current]} $token"
      else
        opts["$current"]="$token"
      fi
    fi
  fi
done

# Reconstruct the final arguments string.
final_args=""
for flag in "${order[@]}"; do
  # Append a space if not the first flag.
  if [[ -n "$final_args" ]]; then
    final_args="$final_args "
  fi
  final_args="$final_args$flag"
  if [[ -n "${opts[$flag]}" ]]; then
    final_args="$final_args ${opts[$flag]}"
  fi
done

echo "$final_args"
