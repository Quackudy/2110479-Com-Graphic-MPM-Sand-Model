# Vulkan graphics starter

A small C++20 / Vulkan 1.3 project with an application loop, an editable CPU
scene, a 3D mesh renderer, and separate Vulkan infrastructure. Two objects share
one colored cube mesh: the left rotates around X/Y and the right stays still.
The renderer uses dynamic rendering, synchronization2, a perspective camera,
depth testing, and up to 4× multisample anti-aliasing (MSAA). Press **Esc** to close the window.

Start with the [beginner walkthrough](GRAPHICS_WALKTHROUGH.md) for the graphics
pipeline, Vulkan concepts, and model/view/projection matrices used in this code.

## Build and run

Prerequisites: C++20 compiler, CMake 3.21+, Vulkan 1.3 SDK/driver with Vulkan-Hpp
headers and dynamic rendering/synchronization2 features, `glslc`, GLFW 3.3+, and
GLM headers/CMake package. GLM supplies standard graphics matrix mathematics;
there is no dependency download during the build. Install Vulkan validation
layers for development; validation and synchronization validation are enabled
when available.

```sh
./run.sh
./run.sh --frames 120
ctest --test-dir build --output-on-failure
bash tests/render_smoke.sh
```

Run from a graphical desktop session. `run.sh` builds C++ and GLSL, then launches
it; run it again after editing. Generated files live in `build/`; edit shader
source rather than `.spv` output. There is no hot reload. On a hybrid laptop,
`__NV_PRIME_RENDER_OFFLOAD=1 ./run.sh` requests the NVIDIA render path.

## Structure and edit points

```text
src/
  main.cpp                    Window, input, timing, application loop
  scene.hpp/.cpp              Geometry, XYZ transforms, camera, M/V/P math
  renderer.hpp/.cpp           GPU meshes, descriptors, pipeline, object draws
  vulkan_context.hpp/.cpp      Device, uploads, swapchain, depth image, synchronization
shaders/
  mesh.vert                   P * V * M * vertex
  mesh.frag                   Fragment colors/effects
```

The application updates CPU scene state and then records drawing:

```cpp
scene::updateScene(scene, deltaTime);
renderer.draw(scene);
```

The scene has no Vulkan handles. The renderer handles drawing and the Vulkan
context handles resource allocation, frame preparation, submission, and presentation.

| Edit point | What to change |
| --- | --- |
| `scene.cpp`: `makeMesh()` | XYZ vertex positions, RGB colors, triangle indices |
| `scene.cpp`: `createScene()` | Objects, initial transforms, camera, background |
| `scene.cpp`: `updateScene()` | Animation using deltaTime in seconds |
| `scene.cpp`: `modelMatrix()` | Model transform order |
| `scene.cpp`: `viewMatrix()` / `projectionMatrix()` | Camera mathematics |
| `mesh.vert` | Vertex processing |
| `mesh.frag` | Fragment colors |
| `renderer.cpp`: `createPipeline()` | Vertex attributes, topology, depth, blending |

## XYZ transforms and model/view/projection

Each object refers to a mesh and has its own transform:

```cpp
struct Transform {
    glm::vec3 position{0, 0, 0};
    glm::vec3 scale{1, 1, 1};
    glm::vec3 rotation{0, 0, 0}; // X/Y/Z Euler angles in radians
};
```

The world is right-handed: X points right, Y up, and positive Z toward the
default camera. The camera starts at `(0, 0, 5)`, looking at `(0, 0, 0)` with
up `(0, 1, 0)`. Its vertical field of view is 60 degrees, near plane 0.1, and far
plane 100. Edit `scene.camera` in `createScene()` to change those values.

The CPU builds three matrices using GLM:

```text
M = T * Rz * Ry * Rx * S      per object
V = lookAtRH(eye, target, up) shared camera
P = perspectiveRH_ZO(fov, framebufferWidth / framebufferHeight, near, far)
P[1][1] *= -1                Vulkan viewport Y correction
```

The vertex shader applies them to a column vector, rightmost operation first:

```glsl
gl_Position = frame.projection * frame.view * object.model * vec4(inPosition, 1.0);
```

Model transforms scale, rotate around X, then Y, then Z, and translate. View
moves world positions into camera space. Projection produces clip positions;
the GPU then divides by W. This uses Vulkan's zero-to-one depth range, with a
positive-height viewport and a projection Y flip so world Y appears upward.
The projection aspect ratio is recalculated from the drawable image size each
frame, preventing stretching on resize.

Euler angles keep this starter easy to edit. They can exhibit gimbal lock;
quaternions are a later step for unrestricted orientation controls.

Add another copy of the uploaded mesh in `createScene()`:

```cpp
scene.objects.push_back({0, {{0, 1.3f, -1}, {0.4f, 0.4f, 0.4f}, {0, 0, 0}}});
```

Change animation in `updateScene()`:

```cpp
scene.objects[0].transform.rotation[1] += deltaTime; // rotate about Y
```

The default also rotates around X at half that speed. Remove the original
animation lines when replacing them, to avoid applying an update twice.
Geometry stays unchanged while objects move.

## Meshes describe shapes; objects place them

The default cube has eight XYZ/RGB vertices and 36 indices: twelve triangles,
two per face. All objects can share its vertex/index buffers.

For a flat rectangle in 3D, replace `makeMesh()`'s return value with:

```cpp
return {
    {
        {{-0.5f, -0.5f, 0}, {1, 0, 0}},
        {{ 0.5f, -0.5f, 0}, {0, 1, 0}},
        {{ 0.5f,  0.5f, 0}, {0, 0, 1}},
        {{-0.5f,  0.5f, 0}, {1, 1, 0}},
    },
    {0, 1, 2, 2, 3, 0}
};
```

Every three indices describe a triangle. Validation rejects incomplete triangles,
invalid indices, and non-finite data. Camera validation rejects invalid frusta,
coincident eye/target, and an up vector parallel to the viewing direction.

Meshes are static after renderer construction. Edit and restart to change
geometry. Adding/removing meshes at runtime requires rebuilding the renderer;
changing CPU vertices does not re-upload them. Objects may reference any
already uploaded mesh and change transforms at runtime.

## GPU data and frame flow

```text
CPU mesh                    -> staged upload -> vertex/index buffers
scene.camera + image aspect -> V/P matrices  -> shared frame uniform buffer
object.transform            -> M matrix      -> push constants before each draw
                                               |
                              vertex shader: P * V * M * position
                                               |
                              fragments + depth test -> swapchain image
                                               |
                                           presentation
```

The vertex stride is 24 bytes. Location 0 reads XYZ at byte 0; location 1 reads
RGB at byte 12. Both use vertex-buffer binding 0. Vertex output location 0
matches fragment input location 0 for interpolated color.

The frame uniform buffer at descriptor set 0/binding 0 contains view at byte 0,
projection at byte 64, and elapsed time at byte 128. Its std140-compatible C++
allocation is 144 bytes. The 64-byte model matrix is pushed before each draw.
Compile-time assertions check those sizes and offsets. Both shaders declare the
same frame block; elapsed time is available for procedural effects.

The context waits for the previous frame fence, acquires a swapchain image,
records attachment layout barriers, and begins dynamic rendering with the
multisampled color image and matching depth image. The color attachment resolves
into the acquired, single-sample swapchain image. Color is cleared to the background; depth is cleared to 1.0.
Depth comparison is `less`, with depth writes enabled, so nearer opaque surfaces
win regardless of draw order. Depth contents are discarded after rendering;
the resolved color is stored for presentation. Blending and back-face culling are disabled.

After draws, the context ends rendering, transitions color to presentation
layout, submits with `submit2`, and presents. Semaphores coordinate acquisition
and presentation; a fence protects CPU reuse. Startup uploads use copy-to-input
barriers and finish before staging memory is freed.

No render-pass or framebuffer objects are created. The pipeline declares color
and depth formats through `PipelineRenderingCreateInfo`. Swapchain recreation
rebuilds image views and the color/depth attachments at the new size. A generation change
makes the renderer rebuild the pipeline against the current formats. Meshes and
descriptors survive. Shutdown waits for GPU work before releasing resources.

## Multisampling

The starter requests 4× MSAA, falling back to 2× or 1× according to the GPU's
framebuffer limits and the actual color/depth formats. Startup output reports
`MSAA: 4x` (or the selected fallback). Selection is in `chooseSampleCount()` in
`vulkan_context.cpp`; pipeline and attachment sample counts stay in sync.

With MSAA enabled, each pixel has multiple coverage/depth samples. Rendering
writes a shared multisampled color image, then dynamic rendering averages those
samples into the single-sample swapchain image. The depth image uses the same
sample count. Both attachments are recreated on resize. At 1×, rendering goes
directly to the swapchain without a resolve image.

This smooths geometry edges at the cost of extra attachment memory and GPU work.
Sample shading remains disabled; MSAA does not solve every kind of shader or
texture aliasing. No extra device feature is needed for ordinary MSAA.

## Scope and checks

This is a small indexed 3D starter, not a full engine. It uses one frame in
flight, shared color/depth attachments, and one allocation per buffer/image. Concurrent
frames would need protected or duplicated frame/attachment resources. Many resources
would benefit from suballocation. Textures, normals/lighting, input-driven
camera controls, compute, and simulation are future work.

CPU tests cover independent animation, invalid scene/camera data, transform
order, camera orientation, Vulkan near/far depth mapping, projection Y correction,
aspect ratio, and perspective division. SPIR-V validation checks compiled shaders.
`tests/render_smoke.sh` renders 120 frames and requires validation/synchronization
validation with no reported warnings/errors; it needs a desktop display.
Inspect both cubes and test resize/minimize/restore on your laptop. CPU and
SPIR-V checks do not replace runtime Vulkan validation or visual checks.

References: [GLM manual](https://github.com/g-truc/glm/blob/master/manual.md),
[Khronos depth buffering](https://docs.vulkan.org/tutorial/latest/07_Depth_buffering.html),
[dynamic rendering](https://docs.vulkan.org/samples/latest/samples/extensions/dynamic_rendering/README.html),
[synchronization2 examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html),
[push constants](https://docs.vulkan.org/guide/latest/push_constants.html).
