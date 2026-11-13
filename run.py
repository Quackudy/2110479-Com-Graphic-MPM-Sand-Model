#!/usr/bin/env python3
"""Bootstrap wrapper: check tools, fetch assets, build, then run the binary.

Command documentation lives in the binary (`./run.py --help`); `test` is
handled here. Particle data is validated by SimulationConfig::loadParticles.
Works on Linux and Windows (`py run.py ...`).
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
AUTHOR = ROOT / "third_party" / "CK-MPM"
ASSETS = [
    "asset/particle_data/sand_armadillo_0_jittered.bin",
    "asset/particle_data/sand_armadillo_1_jittered.bin",
]
BUILD = ROOT / "build"
CONFIG = "Release"
BINARY = BUILD / ("ck_mpm_starter.exe" if os.name == "nt" else "ck_mpm_starter")


def run(command: list[str], cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
    _ = subprocess.run(command, cwd=cwd, env=env, check=True)


def check_prerequisites() -> None:
    """Verify the tools this script itself calls.

    Libraries (Vulkan, GLFW, GLM, nlohmann_json) and the C++ compiler are
    checked by CMake with its own messages.
    """
    tools = ("git", "git-lfs", "cmake", "glslc")
    missing = [tool for tool in tools if not shutil.which(tool)]
    if missing:
        names = " ".join(missing)
        sys.exit(f"error: missing required tools: {names} (glslc comes with the Vulkan SDK)")


def assets_hydrated() -> bool:
    """git-lfs marks hydrated files with '*' and pointer stubs with '-'."""
    if not (AUTHOR / "include" / "mpm_material.cuh").exists():
        return False
    listing = subprocess.run(
        ["git", "lfs", "ls-files", "--include=" + ",".join(ASSETS)],
        cwd=AUTHOR, capture_output=True, text=True, check=True).stdout
    return sum(" * " in line for line in listing.splitlines()) == len(ASSETS)


def setup_assets() -> None:
    """Fetch the author's CK-MPM submodule and only the particle files we need.

    Non-recursive on purpose: the author's nested submodules are not required.
    Skipped when the files are already present, so reruns work offline.
    """
    if assets_hydrated():
        return
    skip_smudge = {**os.environ, "GIT_LFS_SKIP_SMUDGE": "1"}
    submodule = str(AUTHOR.relative_to(ROOT))
    run(["git", "submodule", "update", "--init", submodule], env=skip_smudge)
    run(["git", "lfs", "pull", "--include=" + ",".join(ASSETS)], cwd=AUTHOR)


def build() -> None:
    run(["cmake", "-S", ".", "-B", "build", f"-DCMAKE_BUILD_TYPE={CONFIG}"])
    jobs = str(os.cpu_count() or 1)
    run(["cmake", "--build", "build", "--config", CONFIG, "-j", jobs])


def main(args: list[str]) -> int:
    check_prerequisites()
    setup_assets()
    build()
    if args[:1] == ["test"]:
        # Extra arguments go to ctest, e.g. `./run.py test -LE gpu` for CPU-only checks.
        run(["ctest", "--test-dir", "build", "-C", CONFIG, "--output-on-failure", *args[1:]])
        return 0
    code = subprocess.run([str(BINARY), *args], cwd=ROOT).returncode
    return 128 - code if code < 0 else code  # killed by signal -> shell convention


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
    except KeyboardInterrupt:
        sys.exit(130)
