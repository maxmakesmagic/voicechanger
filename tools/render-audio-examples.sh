#!/usr/bin/env bash

set -euo pipefail

if [[ $# -gt 1 ]]; then
  echo "usage: $0 [path-to-audio_example_renderer]" >&2
  exit 64
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repository_root=$(cd -- "$script_dir/.." && pwd)
renderer=${1:-"$repository_root/build/audio_example_renderer"}
audio_dir="$repository_root/docs/audio"

if [[ ! -x "$renderer" ]]; then
  echo "error: renderer is not executable: $renderer" >&2
  echo "build it with: cmake --build build --target audio_example_renderer" >&2
  exit 1
fi

"$renderer" pitch --ratio 1.25 \
  "$audio_dir/source.wav" "$audio_dir/pitch-up-1.25.wav"
"$renderer" pitch --ratio 0.80 \
  "$audio_dir/source.wav" "$audio_dir/pitch-down-0.80.wav"
"$renderer" chorus \
  --delay-ms 18.0 \
  --depth-ms 8.0 \
  --rate-hz 0.8 \
  --wet 0.6 \
  "$audio_dir/source.wav" "$audio_dir/chorus.wav"

echo "Rendered audio examples in $audio_dir"
