#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <fqbn>" >&2
  exit 64
fi

fqbn=$1
fqbn_key=${fqbn//[^a-zA-Z0-9._-]/_}
build_root=${ARDUINO_BUILD_ROOT:-build/arduino}
sketch_count=0

# An Arduino sketch directory must contain a primary .ino file with the same
# name as the directory. Discover root-level effect directories so a newly
# added effect is compiled by CI without another workflow edit.
while IFS= read -r -d '' sketch_dir; do
  sketch_name=${sketch_dir##*/}
  primary_file="$sketch_dir/$sketch_name.ino"

  if [[ ! -f "$primary_file" ]]; then
    echo "error: $sketch_dir has .ino files but no $primary_file" >&2
    exit 1
  fi

  echo "Compiling $sketch_dir for $fqbn"
  build_path="$build_root/$fqbn_key/$sketch_name"
  mkdir -p "$build_path"
  arduino-cli compile \
    --fqbn "$fqbn" \
    --warnings all \
    --build-path "$build_path" \
    "$sketch_dir"
  sketch_count=$((sketch_count + 1))
done < <(
  find . -mindepth 2 -maxdepth 2 -type f -name '*.ino' -printf '%h\0' |
    sort -zu
)

if [[ $sketch_count -eq 0 ]]; then
  echo "error: no Arduino sketch directories found" >&2
  exit 1
fi

echo "Compiled $sketch_count Arduino sketch(es) for $fqbn"
