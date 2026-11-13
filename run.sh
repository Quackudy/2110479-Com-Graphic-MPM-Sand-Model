#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Fetch the author's CK-MPM submodule and only the particle files we need.
# Non-recursive on purpose: the author's nested submodules are not required.
author=third_party/CK-MPM
if [ ! -e "$author/include/mpm_material.cuh" ]; then
  GIT_LFS_SKIP_SMUDGE=1 git submodule update --init "$author"
fi
(cd "$author" && git lfs pull \
  --include="asset/particle_data/sand_armadillo_0_jittered.bin,asset/particle_data/sand_armadillo_1_jittered.bin")
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2
exec ./build/ck_mpm_starter "$@"
