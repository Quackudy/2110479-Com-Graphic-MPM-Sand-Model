#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-fixture}
case "$mode" in play|fixture) ;; *) echo "usage: $0 [fixture|play]" >&2; exit 1 ;; esac
log=$(mktemp)
trap 'rm -f "$log"' EXIT
if [[ "$mode" == fixture ]]; then
    ./build/particle_render_test >"$log" 2>&1 || { cat "$log"; exit 1; }
else
    ./build/ck_mpm_starter play --frames 120 >"$log" 2>&1 || { cat "$log"; exit 1; }
fi
cat "$log"
if ! rg -q 'validation: on \| sync validation: on' "$log" ||
   ! rg -q 'MSAA: (1|2|4)x' "$log" ||
   ! rg -q 'Processed 120 frame\(s\)\.' "$log" ||
   rg -q '\[Vulkan validation\]|Error:' "$log"; then
    echo "Rendering smoke check failed: require 120 frames with clean validation." >&2
    exit 1
fi
