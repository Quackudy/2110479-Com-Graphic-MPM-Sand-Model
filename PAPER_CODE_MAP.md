# CK-MPM paper → code guide

This guide connects the current Vulkan implementation to **CK-MPM: A Compact-Kernel
Material Point Method**, Michael Liu, Xinlei Wang and Minchen Li, ACM TOG 44(4),
2025. Section and equation numbers refer to the main paper:
[DOI: 10.1145/3731155](https://doi.org/10.1145/3731155).
The local copy is `../CK-MPM Paper.pdf`.

The implementation follows the author's **affine CUDA transfer** and sand
material. A paper reference below identifies the corresponding idea; differences
from the printed equations are called out explicitly.

## Where to start

1. [shaders/interpolation.glsl](shaders/interpolation.glsl): compact kernel,
   grid offsets, weights and particle moment matrix.
2. [shaders/g2p.comp](shaders/g2p.comp): fit local affine motion and update particles.
3. [shaders/p2g.comp](shaders/p2g.comp): distribute mass, affine momentum and stress.
4. [shaders/material.glsl](shaders/material.glsl): sand stress and plastic correction.
5. [src/simulation.cpp](src/simulation.cpp), `Simulation::advance()`: dispatch order,
   synchronization and timings.

## Symbols used in the paper and code

| Meaning | Paper notation | Code |
|---|---|---|
| Particle position and velocity | `x_p`, `v_p` | `Particle.position.xyz`, `velocity.xyz` |
| Grid spacing and numerical timestep | `Δx`, `Δt` | `params.step.y`, `params.step.x` |
| Particle mass and reference volume | `m_p`, `V_p` | `params.step.z`, `params.step.w` |
| Grid mass and momentum/velocity | `m_i`, `m_i v_i`, `v_i` | `nodes[i].w`, `nodes[i].xyz`; XYZ changes from momentum to velocity in grid update |
| Deformation gradient | `F_p` | `f0`, `f1`, `f2`, columns of `deformation(p)` |
| First Piola stress | `P_p` | Computed inside `sandMaterial()`; stored stress is **`P Fᵀ`**, not P alone |
| Local affine velocity | `C_p = B_p D_p⁻¹` | `c0`, `c1`, `c2`, columns of `affine(p)` |
| Particle moment matrices | `B_p`, `D_p` | Local `B`, `D` in G2P; `inverseMoment()` in P2G |
| Dual-grid weight | `w_i,p,k` | `kernelWeight(o, w)` |
| Plastic volume history | Material-specific | `plastic.x`, passed as `logJp` |

The CPU layout is [src/simulation_config.hpp](src/simulation_config.hpp),
`Particle` / `Parameters`; its matching GPU layout and matrix accessors are in
[shaders/mpm_common.glsl](shaders/mpm_common.glsl). Matrices use padded columns.

## Paper-to-code map

| Paper | Implementation | What to look for |
|---|---|---|
| §4.1, Eq. (4): compact 1D kernel | [interpolation.glsl](shaders/interpolation.glsl), `interpolation()` | `1 - d + sin(2πd)/(2π)` for the left node and `1-w` for the right node. |
| §4.2, Eq. (5): multidimensional kernel | Same file, `kernelWeight()` | Product of the X, Y and Z weights. |
| §4.2: dual-grid construction and consistency | Same file, `interpolation()` / `nodeOffset()`; [mpm_common.glsl](shaders/mpm_common.glsl), `nodeIndex()` | Two grids shifted by −¼ and +¼ of a cell along each axis. Each particle visits 8 nodes per grid, 16 total in 3D. |
| §4.3.1, Eq. (12): grid mass | [p2g.comp](shaders/p2g.comp) | `atomicAdd(nodes[i].w, weight * mass)` on each grid. |
| §4.4.1, Eq. (20): affine momentum transfer | Same file | Local affine velocity contributes `mass * C * r` in addition to particle translation. |
| §4.4.1: dual-grid D matrix preceding Eq. (20) | [interpolation.glsl](shaders/interpolation.glsl), `inverseMoment()`; [g2p.comp](shaders/g2p.comp) | Weighted outer products of node-to-particle offsets from both grids. |
| §4.4.2, Eq. (21): affine B matrix | [g2p.comp](shaders/g2p.comp) | `B += outerProduct(u, weight*r)`, followed by the fit `C = B*inverse(D)/dx`. |
| §4.3.2, Eq. (14): stress from material response | [material.glsl](shaders/material.glsl), `sandMaterial()` | Evaluate sand stress after correcting elastic stretches; produce `P Fᵀ`. The detailed sand return mapping comes from author source, below. |
| §4.3.2 force computation; §4.5 MLS compatibility | [p2g.comp](shaders/p2g.comp), `contribution` | Follows the author's CUDA moment-based stress transfer, detailed below; this is not a literal kernel-gradient implementation of Eq. (15). |
| §4.3.3, Eq. (16): grid update | [grid_update.comp](shaders/grid_update.comp) | Divide momentum by mass, add gravity and apply sticky boundaries independently on each grid. Stress momentum was already added during P2G. |
| §4.3.4: return grid motion to particles | [g2p.comp](shaders/g2p.comp) | Gather weighted grid velocity from both grids and average it. See the Eq. (17) distinction below. |
| §3.2 and §4.3.5, Eq. (19): deformation/advection | Same file | `F_new = (I + dt*C)*F_old` and `x_new = x_old + dt*v_new`, using the fitted affine C. |
| §6.2, “Sand Armadillos” | [material.glsl](shaders/material.glsl), [material_update.comp](shaders/material_update.comp), [simulation_config.cpp](src/simulation_config.cpp), `loadParticles()` | Drucker–Prager sand, plastic volume history, two armadillo assets and opposing initial Z velocities. |
| §5: implementation and GPU execution | [simulation.cpp](src/simulation.cpp), `recordStage()` / `advance()` | Separate Vulkan compute dispatches, barriers and timestamp queries; this is not the paper's fused sparse CUDA implementation. |

## Kernel and affine transfer in equations

For one axis, `d` is a particle's cell-local coordinate on a shifted grid:

```
w0 = 1 - d + sin(2πd)/(2π)
w1 = 1 - w0
w_xyz = w_x * w_y * w_z
```

This evaluates Eq. (4) within the occupied cell and uses the partition-of-unity
identity in Eq. (8). The two offset grids are both needed for the compact-kernel
construction. The quadratic branch in `interpolation()` supplies the alternative
B-spline kernel for comparisons.

Using physical node offset `r = x_i - x_p` and updated grid velocity `u_i`, define:

```
D = (1/2) Σ_grids Σ_nodes w * r rᵀ
B = (1/2) Σ_grids Σ_nodes w * u_i rᵀ
C = B D⁻¹
```

The G2P shader accumulates B and D without the halves, since they cancel in
`B D⁻¹`. It uses offsets in cell units for numerical scaling, so its final C
has an additional division by `dx`. `inverseMoment()` returns the inverse of
the averaged D in physical units and evaluates it at the current particle
position for P2G.

Our P2G shader combines affine momentum and stress:

```
T = P Fᵀ
A = m*C - dt*V*D⁻¹*T
grid_momentum += w * (m*v + A*r)
grid_mass     += w * m
```

In particular, **D⁻¹ multiplies T on the left**, matching `contribution` in the
author's [`G2P2G()` implementation](../CK-MPM/include/mpm_algorithm.cuh).
The general MLS basis/shape functions in Eqs. (22)–(25) explain the moment-based
approach; this code uses the author's concrete formulation rather than building
a general polynomial basis or evaluating those expressions symbolically.

For particle velocity, our code uses `v_new = (1/2) Σ w*u_i`, also following the
author's CUDA G2P2G loop. **Printed Eq. (17) includes grid-mass/particle-mass
factors that this shader does not use.** Treat §4.3.4 as the conceptual pipeline
mapping, not a claim that our shader literally evaluates the printed Eq. (17).
Similarly, the fitted C supplies the deformation update rather than explicitly
summing kernel derivatives as in Eq. (18).

## Where the sand model comes from

The paper identifies the Drucker–Prager sand model and experiment parameters
in §6.2. The detailed SVD and plastic return-mapping code is traced to
[CK-MPM/include/mpm_material.cuh](../CK-MPM/include/mpm_material.cuh),
`ComputeStress<kDruckerPragerStvkhencky>`, and its callers in
[mpm_algorithm.cuh](../CK-MPM/include/mpm_algorithm.cuh).

Our [material.glsl](shaders/material.glsl) contains:

- `svd3()`: factor F into rotation/stretch components using one-sided Jacobi sweeps.
- `sandMaterial()`: compute logarithmic stretches, project plastic strain,
  update `logJp`, reconstruct F and evaluate stress.

[material_update.comp](shaders/material_update.comp) writes corrected F, stress
and plastic state back to each particle. Friction angle is configurable; the
author's corresponding CUDA overload fixes its yield coefficient at 30°.
The same mathematical rule does not imply the same material settings.

The paper's sand example uses 511,902 particles, `E=10000`, `ν=0.4`, friction
angle 30° and gravity −2. Our JSON configurations can differ. `loadParticles()`
uses the author assets with initial velocities `(0,0,-0.5)` and `(0,0,0.5)`;
`parameters()` rescales sampled particle mass/volume to preserve total volume.

## Execution order and implementation choices

[Simulation::advance()](src/simulation.cpp) executes each substep as:

```
clear grid → P2G → grid update → G2P → material/plastic update
```

Corrected stress from the end of a substep is used by the next P2G. Initial F
is identity and initial stored stress is zero. Advection and the elastic F
update are inside G2P. This pass arrangement is distinct from the author's
fused G2P2G kernel and sparse, blocked AoSoA storage in §5.1. Our grid is dense,
particle state is an array of padded structs, and P2G uses direct global atomics.
These are implementation differences relevant to performance comparisons.

Automatic acoustic stepping lives in `SimulationConfig::timestep()`. Offline
scheduling also limits particle travel; manual live stepping in `live()` uses
the positive dt and `steps_per_frame` from `live.json` directly. The latter is
an application control, not a stability or real-time guarantee from the paper.

## Tests and application infrastructure

§6.1 studies full momentum-conservation experiments, and §6.2 compares kernels.
Our [tools/evaluate.py](tools/evaluate.py) instead checks small numerical
fixtures: independent NumPy stages, analytic affine fields, two-step state
reuse and GPU safety reductions. [tools/taichi_reference.py](tools/taichi_reference.py)
is our affine stage reference; [tools/cuda_material_reference.cu](tools/cuda_material_reference.cu)
executes the author's material function/SVD. These are implementation checks,
not reproductions of the paper's full conservation or timing experiments.

The following support the application rather than implementing a paper equation:

| Code | Purpose |
|---|---|
| [particle_health.comp](shaders/particle_health.comp), `Simulation::health()` | Finite-state, domain/stencil and maximum-speed reduction. |
| [renderer.cpp](src/renderer.cpp), [particle.vert](shaders/particle.vert), [particle.frag](shaders/particle.frag) | Particle spheres and camera rendering, including direct reads of live GPU state. |
| [vulkan_context.cpp](src/vulkan_context.cpp) | Vulkan allocation, command submission, presentation and validation. |
| [scene_cache.cpp](src/scene_cache.cpp), `calculate()` / `play()` in [main.cpp](src/main.cpp) | Save sampled positions and replay them. |
