#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

author=third_party/CK-MPM
assets=(
  asset/particle_data/sand_armadillo_0_jittered.bin
  asset/particle_data/sand_armadillo_1_jittered.bin
)

# Verify the tools this script itself calls. Libraries (Vulkan, GLFW, GLM,
# nlohmann_json) are checked by CMake's find_package with its own messages.
check_prerequisites() {
  local missing=() tool
  for tool in git git-lfs cmake glslc python3; do
    command -v "$tool" >/dev/null || missing+=("$tool")
  done
  command -v c++ >/dev/null || command -v g++ >/dev/null || command -v clang++ >/dev/null || missing+=("C++ compiler")
  if ((${#missing[@]})); then
    echo "error: missing required tools: ${missing[*]}" >&2
    exit 1
  fi
}

# Fetch the author's CK-MPM submodule and only the particle files we need.
# Non-recursive on purpose: the author's nested submodules are not required.
setup_assets() {
  if [ ! -e "$author/include/mpm_material.cuh" ]; then
    GIT_LFS_SKIP_SMUDGE=1 git submodule update --init "$author"
  fi
  local include
  include=$(IFS=,; echo "${assets[*]}")
  (cd "$author" && git lfs pull --include="$include")
}

build_and_run() {
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j 2
  exec ./build/ck_mpm_starter "$@"
}

check_prerequisites
setup_assets
build_and_run "$@"
