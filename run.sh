#!/usr/bin/env bash
# Bootstrap wrapper: check tools, fetch assets, build, then run the binary.
# Command documentation lives in the binary (`./run.sh --help`); `test` is
# handled here. Particle data is validated by SimulationConfig::loadParticles.
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
# git-lfs marks hydrated files with '*' and pointer stubs with '-', so reruns
# skip the network once the files are present.
setup_assets() {
  local include hydrated=0
  include=$(IFS=,; echo "${assets[*]}")
  if [ -e "$author/include/mpm_material.cuh" ]; then
    hydrated=$(cd "$author" && git lfs ls-files --include="$include" | grep -c ' \* ' || true)
  fi
  ((hydrated == ${#assets[@]})) && return
  GIT_LFS_SKIP_SMUDGE=1 git submodule update --init "$author"
  (cd "$author" && git lfs pull --include="$include")
}

build() {
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j "$(nproc)"
}

check_prerequisites
setup_assets
build
if [ "${1:-}" = test ]; then
  shift
  # Extra arguments go to ctest, e.g. `./run.sh test -LE gpu` for CPU-only checks.
  exec ctest --test-dir build --output-on-failure "$@"
fi
exec ./build/ck_mpm_starter "$@"
