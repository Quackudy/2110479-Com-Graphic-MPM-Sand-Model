# CK-MPM Vulkan sand

3D sand simulation using native Vulkan compute shaders and particle-sphere
rendering. Both live simulation and offline calculation use affine transfer
following the author's CUDA G2P2G implementation, with Drucker–Prager /
StVK-Hencky sand material.

See [docs/paper_math](docs/paper_math/README.md) for the paper-to-code map and
the mathematics learning guide, including derivations and prerequisite maps.

## Requirements

- C++20 compiler, CMake 3.21+, Vulkan 1.3 SDK and `glslc`.
- GLFW 3.3+, GLM and nlohmann/json 3.10+.
- GPU supporting Vulkan buffer float32 atomic addition and GPU timestamps.
- Python 3 with NumPy for numerical checks.
- A desktop display for interactive viewing; Khronos validation layers for
  the validation checks.
- Git and Git LFS. The author's CK-MPM repo is a submodule in
  `third_party/CK-MPM`; `run.sh` fetches it and only the two sand-armadillo
  particle files it needs.

## Windows (experimental, not yet tested)

The C++ sources avoid POSIX-only calls (`src/platform.cpp` holds the Linux and
Windows variants), but the Windows path has not been built yet. `run.sh` needs
bash, so until a cross-platform launcher exists use these steps in a Developer
PowerShell for Visual Studio:

1. Install Visual Studio Build Tools (C++ workload), CMake, Git with Git LFS,
   Python 3 and the LunarG Vulkan SDK (provides `glslc` and the validation layers).
2. Install the libraries with [vcpkg](https://vcpkg.io): `vcpkg install glfw3 glm nlohmann-json`.
3. Fetch the assets (skip the author's nested submodules):

```powershell
$env:GIT_LFS_SKIP_SMUDGE = 1
git submodule update --init third_party/CK-MPM
git -C third_party/CK-MPM lfs pull --include="asset/particle_data/sand_armadillo_0_jittered.bin,asset/particle_data/sand_armadillo_1_jittered.bin"
```

4. Configure and build:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Known gaps: `CMakeLists.txt` looks for `python3`, which Windows usually lacks, and
`tools/evaluate.py` looks for `.venv-reference/bin/python` instead of
`Scripts/python.exe`.

## Run

Run commands from this folder. `run.sh` configures and builds the application
before launching it.

```sh
./run.sh live                    # Simulate and view using live.json
./run.sh calculate               # Calculate sand.json and save the scene
./run.sh calculate my.json       # Calculate a custom configuration
./run.sh play                    # Replay the saved affine particle scene
./run.sh evaluate                # Run numerical reference checks
```

To calculate and immediately replay:

```sh
./run.sh calculate && ./run.sh play
```

For a bounded viewing run, use `./run.sh live --frames 180` or
`./run.sh play --frames 180`.

## Controls

| Key | Action |
|---|---|
| Tab | Toggle mouse look |
| WASD | Move camera |
| Q / E | Move down / up |
| Space | Pause / resume |
| R / Home | Reset live physics or restart saved playback |
| Left / Right | Seek saved playback by half a second |
| Esc | Close |

## Live configuration

Edit [live.json](live.json), then restart the application. Config changes take
effect at startup without rebuilding. The main stepping controls are:

```json
{
  "particles_per_model": 40000,
  "dt": 0.0004,
  "steps_per_frame": 25
}
```

These entries belong in the existing configuration alongside its material,
grid and asset settings.

| Setting | Meaning |
|---|---|
| `particles_per_model` | Particles sampled from each of the two models; integer 1–255951. Total count is twice this value. |
| `dt` | Simulated seconds per numerical step. A positive value is used exactly in live mode; `0` selects the acoustic CFL timestep. |
| `steps_per_frame` | Physics steps per live update; integer 1–4096. |
| `grid` | Grid resolution along each axis; integer 16–256, subject to GPU buffer limits. |
| `kernel` | `ck` for two staggered grids, or `quadratic` for one grid. |
| `young_modulus` | Material stiffness; positive. |
| `poisson_ratio` | Elastic compression response; greater than −1 and less than 0.5. |
| `density` | Material density; positive. |
| `gravity` | Vertical gravitational acceleration; negative values pull downward. |
| `friction_angle` | Internal sand friction angle in degrees; greater than 0 and less than 90. |
| `cfl` | Factor for automatic timestep selection; greater than 0 and at most 0.5. |
| `boundary` | Thickness of sticky grid boundaries in domain units; at least `2/grid` and less than 0.2. |
| `assets` | Particle asset directory, resolved relative to the configuration file. |

Live mode runs the configured steps at the selected dt as fast as computation
and presentation allow. It selects mailbox or immediate presentation where
available, with FIFO as a platform fallback.

```
simulated seconds/frame = dt × steps_per_frame
simulation speed ≈ dt × steps_per_frame × measured FPS
```

For example, `dt=0.0004` and 25 steps advance 0.01 simulated seconds per frame.
At 60 FPS, simulation advances at approximately 0.6× real time. Increasing the
step count adds work per frame and may reduce FPS. Larger dt values advance
more time per step but can destabilize the solver.

The terminal periodically prints dt, steps/frame, simulated seconds/frame,
measured FPS and simulation speed, followed by a final summary on closing.
The window title also shows simulated time, FPS and speed.

Particles stay in GPU storage and the renderer reads their positions directly.
A GPU reduction checks finite state, domain and grid-stencil bounds and maximum
speed; the CPU reads its 16-byte summary. Invalid state stops the simulation
and reports error flags and the first affected particle index.

## Offline calculation and playback

[sand.json](sand.json) configures offline calculation. It uses the same asset,
material and grid settings, plus:

| Setting | Meaning |
|---|---|
| `seconds` | Requested simulation duration; greater than 0 and at most 300. |
| `fps` | Saved-frame sampling rate; integer 1–240. |
| `dt` | Automatic acoustic CFL step when zero; a positive override must satisfy the CFL limit. A particle-travel limit can reduce the step further. |

`calculate` runs numerical reference checks, simulates the scene and stores
sampled particle positions. It publishes one saved scene at
`output/scene.bin` by atomic replacement after a successful calculation.
A failed calculation preserves the previous completed scene. Only one offline
calculation runs at a time.

`play` streams and interpolates positions from the saved affine scene into
the particle renderer. Playback follows the saved sampling rate and supports
camera movement, pause, restart and seeking.

## Profiling and checks

`output/performance.jsonl` records settings, particle count, GPU information,
physics timings and run results. Five physics passes have GPU timestamp
measurements: clear grid, P2G, grid update, G2P and material update. Live reports
also include GPU-safety-check wall time, step counts, transfer byte counters,
rendered FPS and measured simulation speed. Evaluation results are written to
`output/evaluation.json`.

Run the tests with `run.sh`:

```sh
./run.sh test                   # all tests
./run.sh test -LE gpu           # CPU-only subset (no GPU or display)
./run.sh play --frames 120      # manual saved-scene playback check (after calculate)
```

`particle_render` also requires the Khronos validation layers
(`vulkan-validation-layers` on Arch) and fails without them.

Tests cover camera transforms, configuration validation, saved-scene replacement
and rollback, CK/quadratic transfer stages, affine-state reuse over two steps,
analytic translation/rotation/shear, GPU max-speed reduction and invalid-state
detection. Numerical checks require a suitable GPU; interactive checks also
require a desktop display.

NumPy supplies the CPU stage reference. The Taichi reference uses
`.venv-reference/bin/python` with packages from
[tools/requirements-reference.txt](tools/requirements-reference.txt). When the
CUDA toolkit and author source are available, CMake builds a reference executable
calling the author's material function and SVD. Missing optional references
are reported explicitly. These checks verify implementation behavior; they do
not establish full-scene equivalence to the author's fused CUDA solver.

The author source and assets use Apache-2.0; see
[the included license](licenses/CK-MPM-Apache-2.0.txt). AI assistance is recorded
in [AI_USE.md](AI_USE.md), and the project/prompt log is `../PROJECT_LOG.txt`.
