# From a triangle to the screen: graphics and Vulkan

This guide follows the code in this repository. You do not need to memorize the Vulkan API before understanding what it does. First understand what drawing needs; then attach Vulkan names to those jobs.

The starter renders two colored cubes with a perspective camera and depth testing. Both objects share the same geometry, but each has its own transform. The left object rotates. The renderer uses **Vulkan 1.3 dynamic rendering and synchronization2**, with up to 4× MSAA and one frame in flight.

## Reading route

1. Read sections 1–5 to understand how geometry becomes pixels.
2. Read sections 6–8 to understand how CPU data reaches shaders.
3. Read sections 9–12 to understand Vulkan setup, frames, and synchronization.
4. Try the experiments in section 14, one at a time.

Links to the actual code are relative to this file. Code examples called “conceptual” explain an idea rather than provide a replacement implementation.

## 1. What are we asking the computer to do?

A window ultimately displays a rectangular image: a grid of pixels. Each pixel stores a color. Your job is to produce the next image.

For this scene, you give the computer:

- Eight XYZ vertex positions that describe a cube.
- A color at each vertex.
- Thirty-six indices that connect those vertices into twelve triangles.
- An object's scale, rotation, and position.
- Rules for converting that information into pixel colors.

The GPU runs those rules and writes an image. The window system presents the image.

```text
CPU scene data
    ↓ upload geometry / send frame and object parameters
GPU draws triangles into an image
    ↓ presentation
Window displays that image
```

Graphics APIs such as Vulkan connect your program to those GPU capabilities. They do not automatically supply a scene, camera, animation system, or lighting model. Those are application choices.

### The CPU and GPU have different jobs

The CPU runs `main.cpp`, builds the scene, updates animation, and records commands. The GPU executes submitted commands and runs the shaders.

Recording `drawIndexed(...)` does not immediately draw a triangle. It puts a drawing instruction into a command buffer. Queue submission makes that recorded work available for GPU execution.

CPU execution and GPU execution can overlap. This is why Vulkan needs explicit rules about when memory may be rewritten or resources destroyed.

## 2. Meshes, vertices, indices, and objects

Start with [scene.hpp](src/scene.hpp) and [scene.cpp](src/scene.cpp).

```cpp
struct Vertex {
    std::array<float, 3> position;
    std::array<float, 3> color;
};
```

A vertex is a record of attributes. In this project it has a 3D position and an RGB color. Other programs might also include a normal, texture coordinates, or skinning weights.

A **mesh** is geometry. An **object** is one use of that geometry in the scene.

```text
Mesh 0: cube vertices + indices (12 triangles)
    ├── Object 0: left position, its own rotation
    └── Object 1: right position, its own rotation
```

`Object.mesh` is an index into `Scene::meshes`. It is an application index, not a Vulkan binding or shader location.

### Why have indices?

The default cube has 36 indices, beginning with:

```cpp
{0, 1, 2, 2, 3, 0, /* other faces */}
```

With triangle-list topology, every group of three indices describes a triangle. This group selects vertices 0, 1, and 2.

A rectangle can use four vertices and six indices:

```text
0 ───── 1
│     / │    triangles: (0, 1, 2) and (2, 3, 0)
│   /   │    indices:   0, 1, 2, 2, 3, 0
3 ───── 2
```

Indices allow neighboring triangles to share vertices. An index is not a position; it selects a vertex record.

The **vertex buffer** holds vertex records. The **index buffer** holds integers selecting those records. Neither one is a command buffer.

## 3. The generic graphics pipeline

For this starter, think of drawing as this conceptual sequence:

```text
Fetch vertex attributes using indices
    ↓
Vertex shader: transform positions, output vertex attributes
    ↓
Assemble triangles and clip them to the visible volume
    ↓
Divide by w and map coordinates through the viewport
    ↓
Rasterize: determine covered samples and interpolate attributes
    ↓
Fragment shader: calculate colors
    ↓
Fragment tests / color writes / optional blending
    ↓
Target image
```

This is a learning diagram, not a guarantee that the hardware executes every operation strictly one after another. GPUs overlap work, and some fragment tests can happen before shading.

The programmable stages here are the vertex and fragment shaders. The other operations are provided by the GPU and configured by pipeline state. The [Khronos pipeline overview](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/02_Graphics_pipeline_basics/00_Introduction.html) illustrates this distinction.

### Vertex shader

A vertex shader operates on vertex inputs. It calculates the position used for drawing and can output attributes for interpolation. It does not fill the triangle with pixels itself.

[mesh.vert](shaders/mesh.vert) transforms `inPosition` and writes `gl_Position`. It also passes the vertex color onward.

Indexed drawing can reuse processed vertices. Do not assume one shader invocation per index or rely on invocation order.

### Clipping

A triangle that extends outside the visible volume can be cut at the boundary. A completely outside triangle may disappear. Clipping occurs before perspective division.

### Rasterization and interpolation

Three vertices describe the triangle's corners. Rasterization determines which image samples the triangle covers.

The GPU interpolates the vertex outputs at fragment positions. If one corner is red and another green, points between them receive mixtures of those colors. The vertex shader does not contain a loop calculating every mixture.

For example, at equal weights between pure red, green, and blue vertices, the interpolated RGB value is approximately `(1/3, 1/3, 1/3)`. The default shader interpolation accounts for perspective, using the vertex clip positions' W values. Here those values depend on distance from the camera.

### Fragment shader

A fragment is a candidate contribution to an image sample. It is not necessarily a final pixel: it can fail a test, be discarded, blend with an existing color, or be overwritten later.

[mesh.frag](shaders/mesh.frag) does:

```glsl
outColor = vec4(color, 1.0);
```

It takes the interpolated RGB color and adds alpha `1.0`.

### Depth and blending

Depth testing helps decide which surface is in front. Blending combines a new color with a stored color, often for transparency.

This starter has a depth image, clears depth to 1.0, and enables depth writes with a `less` comparison. Nearer surfaces win over farther surfaces even if drawn first. Blending is disabled; setting alpha below 1 alone does not enable transparency. Depth buffering makes cube faces occlude each other correctly.

## 4. Coordinates and the M, V, P matrices

Coordinates only make sense when you know their space. This scene uses a right-handed world: X right, Y up, and positive Z toward the default camera.

| Space | Meaning | In this starter |
| --- | --- | --- |
| Local/model | Relative to the mesh origin | XYZ positions in `makeMesh()` |
| World | Relative to the scene | Result of M times local position |
| View | Relative to the camera | Result of V times world position |
| Clip | Homogeneous position before division by W | Result of P times view position |
| Normalized device coordinates (NDC) | Clip XYZ divided by W | Used for viewport/depth mapping |
| Framebuffer | Continuous image coordinates | Result of viewport mapping |

The actual vertex shader says:

```glsl
gl_Position = frame.projection * frame.view * object.model * vec4(inPosition, 1.0);
```

Read from the right: **local → M → world → V → camera → P → clip**.

### M: model matrix places an object

[modelMatrix()](src/scene.cpp) calculates:

```text
M = T * Rz * Ry * Rx * S
```

Matrix multiplication is not commutative. With column vectors, the rightmost operation happens first: scale, rotate around X, then Y, then Z, then translate. Rotation is around the object's local origin. All angles are radians; one full turn is 2π.

The code uses GLM's matrix operations:

```cpp
auto model = glm::translate(glm::mat4(1.0f), transform.position);
model = glm::rotate(model, transform.rotation[2], glm::vec3(0, 0, 1));
model = glm::rotate(model, transform.rotation[1], glm::vec3(0, 1, 0));
model = glm::rotate(model, transform.rotation[0], glm::vec3(1, 0, 0));
return glm::scale(model, transform.scale);
```

Each GLM call appends a multiplication on the right. The call order therefore differs from the order in which the vertex experiences the operations.

Written as rows, translation and scale matrices are:

```text
T = [ 1  0  0  tx ]     S = [ sx  0   0   0 ]
    [ 0  1  0  ty ]         [ 0   sy  0   0 ]
    [ 0  0  1  tz ]         [ 0   0   sz  0 ]
    [ 0  0  0   1 ]         [ 0   0   0   1 ]
```

For a positive right-handed rotation θ about each axis, c = cos(θ), s = sin(θ):

```text
Rx = [ 1  0  0  0 ]   Ry = [ c  0  s  0 ]   Rz = [ c -s  0  0 ]
     [ 0  c -s  0 ]        [ 0  1  0  0 ]        [ s  c  0  0 ]
     [ 0  s  c  0 ]        [-s  0  c  0 ]        [ 0  0  1  0 ]
     [ 0  0  0  1 ]        [ 0  0  0  1 ]        [ 0  0  0  1 ]
```

Each rotation uses its own angle from `rotation[0]`, `[1]`, or `[2]`. Euler angles are easy to inspect but can exhibit gimbal lock. Quaternions would be useful later for unrestricted orientation controls.

### V: view matrix expresses the world relative to a camera

[viewMatrix()](src/scene.cpp) uses:

```cpp
glm::lookAtRH(eye, target, up)
```

The camera defaults to eye `(0, 0, 5)`, target `(0, 0, 0)`, up `(0, 1, 0)`. In camera space, the camera is at the origin and looks down negative Z. Moving the camera does not require rewriting the meshes.

Conceptually, `lookAtRH` constructs camera basis vectors:

```text
f = normalize(target - eye)  (forward)
r = normalize(cross(f, up))  (right)
u = cross(r, f)              (corrected up)

V = [ rx  ry  rz  -dot(r, eye) ]
    [ ux  uy  uz  -dot(u, eye) ]
    [-fx -fy -fz   dot(f, eye) ]
    [  0   0   0       1      ]
```

Eye and target must differ, and up cannot be zero or parallel to the viewing direction. Scene validation checks the camera vectors before drawing.

### P: perspective projection maps the camera volume to clip space

[projectionMatrix()](src/scene.cpp) uses explicit right-handed, zero-to-one-depth GLM math:

```cpp
auto projection = glm::perspectiveRH_ZO(camera.verticalFov, aspect,
                                       camera.nearPlane, camera.farPlane);
projection[1][1] *= -1.0f;
```

`aspect` is drawable framebuffer width divided by height. It is recalculated from the swapchain extent each frame. The projection therefore compensates for window aspect ratio.

For vertical field of view θ, aspect a, near n, far F, and q = 1 / tan(θ/2), this code's projection is:

```text
P = [ q/a   0        0           0       ]
    [  0   -q        0           0       ]
    [  0    0     F/(n-F)     F*n/(n-F)  ]
    [  0    0       -1           0       ]
```

The minus sign on Y compensates for Vulkan's positive-height viewport mapping NDC Y downward, so world Y appears upward. `perspectiveRH_ZO` supplies the depth range; the code does not depend on GLM's default OpenGL projection conventions.

The fourth component of clip position is W, not alpha. Here W equals negative camera-space Z. After clipping, the GPU divides clip XYZ by W. Farther points have larger W, so their projected XY positions are closer to the center. This is the perspective effect.

In the Vulkan conventions used here, visible clip coordinates satisfy:

```text
-W ≤ X ≤ W
-W ≤ Y ≤ W
 0 ≤ Z ≤ W
```

After division, NDC X/Y are in `[-1, +1]` and Z in `[0, 1]`. See [Vulkan vertex post-processing](https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html). Optional extensions can alter some conventions; this starter does not enable them.

For the current positive-height viewport:

```text
pixelX = (ndcX + 1) * width / 2
pixelY = (ndcY + 1) * height / 2
```

These are continuous image coordinates rather than integer pixel indices. NDC Y=-1 maps to the top, while world positive Y maps there because the projection flips it.

### Storage order is different from multiplication order

The displayed matrices above are written as rows for readability. GLM and GLSL store these matrices column-major. `matrix[column][row]` selects an element in GLM. Both sides use the same layout, so the uploaded matrices require no transpose. Do not reverse `P * V * M` because of storage order.

GLM is a header-only graphics math library already available in this environment; CMake now explicitly requires its package. See the [GLM manual](https://github.com/g-truc/glm/blob/master/manual.md).

## 5. Trace the default object's origin through MVP

At the initial state, object 0 has position `(-0.9, 0, 0)`, scale `(0.9, 0.9, 0.9)`, and no rotation. For a 960×720 image, its matrices are approximately:

```text
M = [ .9  0   0  -.9 ]   V = [ 1  0  0   0 ]
    [  0 .9   0   0 ]       [ 0  1  0   0 ]
    [  0  0  .9   0 ]       [ 0  0  1  -5 ]
    [  0  0   0   1 ]       [ 0  0  0   1 ]

P = [ 1.299   0        0        0     ]
    [   0   -1.732     0        0     ]
    [   0     0     -1.001   -.1001   ]
    [   0     0       -1        0     ]
```

Trace the local origin (a reference point, not one of the cube's corner vertices):

```text
local:            ( 0,       0,  0,      1)
after M / world:  (-0.9,     0,  0,      1)
after V / camera: (-0.9,     0, -5,      1)
after P / clip:   (-1.1691,  0,  4.9049, 5)
after divide W:   (-0.2338,  0,  0.9810)
viewport:         approximately (367.8, 360)
```

Every actual cube vertex follows the same path. As the first object rotates, M changes. V stays the same unless you move the camera. P changes if camera lens settings or framebuffer aspect ratio change.

`updateScene()` rotates object 0 around X at 0.5 radians/second and Y at 1 radian/second. It changes the CPU transform, not the mesh. Each frame the renderer builds and pushes the resulting M before that object's draw. Both objects still share the same GPU mesh.

## 6. Locations and bindings: which number connects what?

This is the distinction most likely to cause confusion. Several separate interfaces use small integers, often all starting at zero.

### A. Vertex-buffer binding: select a stream of bytes

In [renderer.cpp](src/renderer.cpp), `createPipeline()` declares:

```cpp
vk::VertexInputBindingDescription vertexBinding{
    0, sizeof(scene::Vertex), vk::VertexInputRate::eVertex
};
```

Binding `0` is a vertex-buffer stream. Its stride is 24 bytes: six 32-bit floats per vertex. `eVertex` means advance through this stream for each vertex, rather than for each instance.

Later, drawing chooses the actual buffer:

```cpp
command.bindVertexBuffers(0, *mesh.vertices.handle, vk::DeviceSize{0});
```

The first `0` selects binding 0. The final `0` is the byte offset into that buffer.

### B. Vertex-input location: identify an attribute

The attribute descriptions are:

```cpp
// location, vertex-buffer binding, format, byte offset within a vertex
{0, 0, vk::Format::eR32G32B32Sfloat, offsetof(scene::Vertex, position)},
{1, 0, vk::Format::eR32G32B32Sfloat, offsetof(scene::Vertex, color)}
```

They match the shader:

```glsl
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
```

For vertex number i, the fetched attribute starts at:

```text
buffer base + bound buffer offset + i * stride + attribute offset
```

Here the position offset is 0 bytes and color offset is 12 bytes.

```text
One vertex: [x y z] [r g b] = 24 bytes
              ↑      ↑
         location 0  location 1
         both fetched from vertex-buffer binding 0
```

**The bytes do not contain location numbers.** The pipeline configuration tells Vulkan how to interpret those bytes. Names such as `position` and `inPosition` do not perform the connection.

### C. Shader-stage locations: connect outputs to inputs

The vertex shader writes:

```glsl
layout(location = 0) out vec3 color;
```

The fragment shader reads:

```glsl
layout(location = 0) in vec3 color;
```

These matching locations connect the vertex output to the fragment input, through interpolation. They are a separate interface from vertex-buffer inputs. Vertex input color can use location 1 while vertex output color uses location 0.

`gl_Position` is a built-in output with special meaning. It does not need a user-assigned location.

### D. Fragment-output location: select a color attachment slot

```glsl
layout(location = 0) out vec4 outColor;
```

This output corresponds to color attachment slot 0. Dynamic rendering supplies one color attachment in this program, so the output goes there.

### E. Descriptor set and binding: locate a shader resource

Both shaders declare:

```glsl
layout(set = 0, binding = 0, std140) uniform FrameUniforms {
    mat4 view;
    mat4 projection;
    float elapsedTime;
} frame;
```

This means descriptor set 0, descriptor binding 0. It points to the frame uniform buffer. It has no relationship to vertex-buffer binding 0 or vertex-input location 0.

| Number | Selects | Set by |
| --- | --- | --- |
| Vertex-buffer binding | A vertex data stream | Binding description + `bindVertexBuffers` |
| Vertex-input location | An attribute shader input | Attribute description + shader `in` |
| Stage output/input location | Data passed between shader stages | Matching shader `out` and `in` |
| Fragment-output location | A color attachment slot | Shader output + rendering attachment list |
| Descriptor set/binding | A buffer/image resource accessible to shaders | Descriptor layouts, sets, and shader declarations |
| Object's mesh index | A mesh in the application's collection | `scene.cpp` |

You may reuse zero across these interfaces because they identify different things.

## 7. Three paths for data to reach shaders

This starter uses three distinct paths:

| Data | Changes how often? | Mechanism |
| --- | --- | --- |
| Vertex positions/colors | Uploaded at renderer construction | Vertex buffer |
| Camera view/projection and elapsed time | Every frame | Uniform buffer via descriptor |
| Model matrix built from XYZ transform | Before each object draw | Push constants |

### Vertex data

`createSceneResources()` uploads each mesh once. Different objects referencing that mesh bind the same GPU buffers.

Editing CPU mesh data after construction does not automatically change GPU memory. Restart after editing geometry; runtime mesh updates need an explicit upload and synchronization path.

### Uniform buffers and descriptors

A uniform buffer stores shader-readable parameters in memory. A descriptor identifies a resource and the range the shader can access.

The descriptor objects in `createSceneResources()` have different roles:

```text
Descriptor set layout: declares binding 0 is one uniform-buffer descriptor
Descriptor pool:      provides allocation capacity for sets
Descriptor set:       contains the actual uniform-buffer reference
Pipeline layout:      declares which set layouts and push constants are allowed
bindDescriptorSets:   selects the set for subsequent drawing
```

Updating a descriptor makes it refer to a buffer. Writing that buffer changes its contents. These are separate operations. Here the descriptor is configured once; camera matrices and elapsed time are written every frame.

The shaders use view and projection from the frame buffer. Elapsed time is declared but not used by default. Rotation comes from the CPU updating the transform and building a model matrix.

The [Khronos descriptor tutorial](https://docs.vulkan.org/tutorial/latest/05_Uniform_buffers/00_Descriptor_set_layout_and_buffer.html) gives a larger example using camera matrices.

### Push constants

Before each draw, the renderer records:

```cpp
const auto model = scene::modelMatrix(object.transform);
command.pushConstants<glm::mat4>(
    *layout, vk::ShaderStageFlagBits::eVertex, 0, model);
```

The values are copied when this command is recorded; it does not store a pointer to the C++ object. Subsequent draws use the current pushed values until changed.

```text
push model matrix A → draw mesh 0
push model matrix B → draw mesh 0
```

Each draw therefore gets its own transform. Push constants are intended for small parameter blocks, and their capacity is limited by the device. See the [Khronos push-constant guide](https://docs.vulkan.org/guide/latest/push_constants.html).

### Byte layout must match

C++ and shader code must agree about offsets and sizes. Vulkan does not interpret C++ field names.

The CPU transform stays in scene memory. Its model matrix is sent as 64 bytes of column-major floats: four columns of four floats. The shader push-constant block declares `mat4 model` at byte 0.

The shared frame block follows `std140`:

```text
view:        byte 0   (64-byte mat4)
projection:  byte 64  (64-byte mat4)
elapsedTime: byte 128 (float)
block size:  144 bytes, including trailing padding
```

`renderer.hpp` checks the frame block's offsets and size. `renderer.cpp` checks the matrix and vertex layouts. Vertex positions and colors are plain packed arrays; do not assume every shader block shares their packing rules.

## 8. Buffers, memory, and uploading

In Vulkan, a buffer is a resource describing a byte range and its intended uses. Device memory is the backing storage. Creating a buffer and binding memory to it are separate steps.

[createBuffer()](src/vulkan_context.cpp) does:

1. Create the buffer with its size and usage flags.
2. Query its memory requirements.
3. Find a compatible memory type.
4. Allocate that memory.
5. Bind it to the buffer.

### Why staging memory?

The CPU needs memory it can map and write. For rendering, the program chooses device-local storage for mesh buffers.

```text
CPU mesh vector
    ↓ memcpy into mapped staging allocation
Host-visible staging buffer
    ↓ GPU copy command
Device-local vertex/index buffer
    ↓ draw reads
Vertex input
```

Host-visible means CPU mapping is permitted. Device-local means local to the device; it does not universally mean “CPU inaccessible,” particularly on integrated GPUs.

This project's staging memory is also host-coherent, so this upload does not need an explicit flush. Coherence does not permit overwriting data while the GPU is using it; synchronization still matters.

After the GPU copy, `uploadBuffer()` records a barrier from copy writes to vertex-attribute or index reads. It then submits and waits for the startup copy to finish before destroying the staging buffer.

The startup wait is deliberately simple. It would be inefficient for a continuous stream of uploads, but this starter uploads its tiny static meshes once.

## 9. Vulkan objects: give each one a job

Read [vulkan_context.hpp](src/vulkan_context.hpp) after understanding the previous sections.

| Object | Job in this program |
| --- | --- |
| Instance | Connect to Vulkan, enumerate GPUs, enable instance extensions/layers |
| Validation layer/debug messenger | Report API misuse and synchronization problems |
| Surface | Represent the window as a presentation destination |
| Physical device | Identify a GPU and query its capabilities |
| Logical device | Enable features and create resources/queues on the chosen GPU |
| Queue family | Identify queues with the capabilities needed here |
| Queue | Receive submissions and presentation requests |
| Command pool | Allocate command buffers for a queue family |
| Command buffer | Hold recorded GPU instructions |
| Swapchain | Manage images suitable for presentation to the surface |
| Image | Store pixel data |
| Image view | Describe how an image or subresource is used |
| Graphics pipeline | Combine shaders with graphics processing settings |
| Pipeline layout | Describe descriptor set layouts and push-constant ranges |
| Semaphore | Coordinate GPU work and acquisition/presentation |
| Fence | Let the CPU wait for submitted GPU work to complete |

A logical device does not create a new physical GPU. “Queue family” is a capability grouping, not a shader stage.

### Feature support and feature enablement

Using Vulkan 1.3 is not enough by itself. `pickDevice()` queries the Vulkan 1.3 feature structure and checks dynamic rendering and synchronization2. `createDevice()` explicitly enables both features.

The program also requires a queue family that supports both graphics and presentation to this surface. Keeping those on one queue family avoids ownership transfers between separate graphics/presentation families.

### C++ wrappers

The code uses Vulkan-Hpp types (`vk::...`) and RAII wrappers (`vk::raii::...`). RAII destroys Vulkan objects when their C++ owners are destroyed. Dereferencing a wrapper, such as `*pipeline`, supplies its underlying handle.

RAII helps with ownership. It does not know when the GPU has finished using a resource. The renderer waits for the device before releasing its resources at shutdown. C++ members are destroyed in reverse declaration order, which is why the context is declared before the resources that depend on it.

## 10. Dynamic rendering and pipeline state

There are two related but different questions:

1. What rules should drawing use?
2. Which image should this drawing write into?

### Graphics pipeline: the rules

`Renderer::createPipeline()` declares:

- Vertex and fragment shader programs.
- Vertex byte layout.
- Triangle-list topology.
- Filled triangles, with face culling disabled.
- Up to four samples per pixel, with hardware-supported fallback.
- RGBA writes, with blending disabled.
- Dynamic viewport and scissor.
- Descriptor/push-constant interface.
- The expected color and depth attachment formats.
- Depth testing/writes with `less` comparison.

Most of these settings are stored in the pipeline. Binding a pipeline chooses those rules for subsequent draws. A different object transform does not require a different pipeline.

### Dynamic rendering: choose the target directly

The pipeline declares its expected color/depth attachment formats through `vk::PipelineRenderingCreateInfo`. It does not reference a `vk::RenderPass`.

At frame recording time, `beginFrame()` supplies:

```text
RenderingAttachmentInfo:
    imageView   = multisampled color view (swapchain view at 1×)
    imageLayout = color-attachment layout
    loadOp      = clear
    storeOp     = dontCare for MSAA source, store at 1×
    clearValue  = scene background
    resolve     = average into acquired swapchain view (when multisampled)

RenderingInfo:
    render area = current image extent
    layers      = 1
    colors      = that attachment
    depth       = the shared depth image view, clear to 1.0
```

Depth is cleared to 1.0, used for depth testing/writes, and discarded after rendering (`storeOp = dontCare`). The shared depth image is safe because only one rendering frame is in flight. Then it records `beginRendering(...)`. After the object draws, it records `endRendering()`.

There are **no Vulkan render-pass or framebuffer objects** in this starter. The target image is still often called a framebuffer in generic graphics discussion; that word does not necessarily refer to a `VkFramebuffer` object.

Older code uses `beginRenderPass()` and framebuffer objects. That is a valid Vulkan path, but dynamic rendering is a simpler starting point for this single-pass renderer. The [Khronos dynamic-rendering sample](https://docs.vulkan.org/samples/latest/samples/extensions/dynamic_rendering/README.html) compares the two approaches.

### The depth image is another rendering attachment

`createAttachmentResources()` selects a supported depth format (`D32Sfloat`, otherwise `D16Unorm`), creates a 2D image at the swapchain extent, allocates device-local memory, and creates a depth-aspect image view. It is not a presentable image and does not belong to the swapchain.

`beginFrame()` transitions it to `DepthStencilAttachmentOptimal` and supplies its view as `pDepthAttachment`. `createPipeline()` declares the same depth format and enables depth testing/writing. The clear value of 1.0 and `less` comparison match our ordinary near=0/far=1 projection convention.

The depth image is recreated when the swapchain size changes. A single shared image is enough for one rendering frame in flight; it must be protected or duplicated before allowing overlapping frames.

### MSAA: soften the stair-step edges

A triangle edge can cover only part of a pixel. One coverage sample gives a harsh covered/uncovered decision. Multisample anti-aliasing (MSAA) uses several coverage samples and resolves their colors to one display pixel, smoothing geometric edges. It adds image storage and rendering cost; it does not automatically run the fragment shader separately for every sample.

`chooseSampleCount()` requests 4×, with 2× or 1× fallback. `createAttachmentResources()` checks the intersection of framebuffer color/depth limits and the chosen formats' supported sample counts. The pipeline, color image, and depth image all use that count.

```text
Rasterization and depth testing at multiple samples per pixel
    ↓
Multisampled color image
    ↓ average resolve during dynamic rendering
Single-sample swapchain image
    ↓
Presentation
```

The color attachment specifies `resolveMode = average` and the acquired swapchain view as `resolveImageView`. The MSAA source is discarded after resolving; the swapchain image holds the result. A barrier prepares the shared MSAA image before each frame. At 1×, drawing uses the swapchain directly and no resolve is configured.

The startup log reports the chosen count. Both color and depth resources are recreated on resize. A single pair is safe because this starter only has one rendering frame in flight.

Ordinary MSAA improves geometry edges; shader/texture aliasing may require other techniques. Sample shading is left disabled. See the [Khronos multisampling chapter](https://docs.vulkan.org/tutorial/latest/10_Multisampling.html).

### Dynamic state is a separate concept

`setViewport()` changes coordinate-to-image mapping. `setScissor()` changes the rectangle in which fragments may be written. This pipeline marks both as dynamic, so they are set while recording commands.

Dynamic rendering does not mean every pipeline setting becomes dynamic, that geometry animates automatically, or that synchronization disappears.

## 11. Follow one complete frame

Start at [main.cpp](src/main.cpp), then follow `Renderer::draw()` and the context's `beginFrame()` / `endFrame()`.

```text
CPU:
  Poll input
  Calculate deltaTime
  updateScene(scene, deltaTime)
  renderer.draw(scene)
      │
      ├── Wait for previous rendering fence
      ├── Acquire a swapchain image
      ├── Begin recording command buffer
      ├── Record color/depth attachment transitions
      ├── Record beginRendering (clear background and depth)
      ├── Build V/P and write shared frame uniform buffer
      ├── Bind pipeline, viewport, scissor, descriptor set
      ├── For each object:
      │     bind vertex/index buffers
      │     build/push that object's model matrix
      │     record drawIndexed
      ├── Record endRendering (resolve MSAA color into swapchain)
      ├── Record transition to presentation layout
      ├── Finish command recording
      ├── Submit with semaphore wait/signal and fence
      └── Request presentation, waiting on rendering semaphore

GPU / presentation system:
  Execute the submitted work when its dependencies allow it
  Present the completed swapchain image
```

Building M/V/P and writing uniforms happen on the CPU. The uniform write happens immediately. Recorded commands execute later. Writing after waiting on the previous frame's fence protects this shared buffer from being overwritten while the previous rendering submission reads it.

`drawIndexed(mesh.indexCount, 1, 0, 0, 0)` means:

| Argument | Value here | Meaning |
| --- | --- | --- |
| indexCount | Mesh's index count | Number of indices to process |
| instanceCount | 1 | Draw one instance |
| firstIndex | 0 | Start at the first index |
| vertexOffset | 0 | Add no offset to fetched indices |
| firstInstance | 0 | Start instance numbering at zero |

This starter uses one draw per object, rather than one instanced draw for all objects.

### Swapchain images and frames in flight are different

The swapchain normally has several images because rendering and display need to exchange images. That does not imply the application has several rendering submissions in flight.

Here there is one command buffer, one shared frame uniform buffer, shared color/depth attachments, and one frame fence. The CPU waits before reusing them. Adding multiple frames in flight would require protecting or duplicating resources that overlap; simply removing the wait is incorrect.

## 12. Synchronization and image layouts

Synchronization solves concrete dependency problems. Begin with “who writes this?” and “who reads or reuses it next?”

### Three mechanisms used here

| Mechanism | What it coordinates | Example |
| --- | --- | --- |
| Fence | CPU waiting on submitted GPU completion | Safe reuse of command buffer and uniform memory |
| Semaphore | GPU/presentation dependencies | Acquire image → render → present |
| Barrier | Execution/memory dependencies and image transitions | Copy writes → vertex reads |

A fence is not an image layout transition. A layout transition is not a replacement for waiting for acquisition.

### Acquire → render → present

```text
Acquire image → signal imageReady
    ↓ rendering submission waits at color-attachment output
Transition image, clear it, draw into it, transition for presentation
    ↓ signal renderDone[imageIndex] after all commands
Presentation waits on renderDone[imageIndex]
```

The frame fence signals completion of the rendering submission. That alone does not guarantee presentation has consumed its semaphore. The code therefore keeps a render-completion semaphore per swapchain image, rather than assuming the rendering fence makes one shared presentation semaphore reusable.

### What is an image layout?

An image layout describes how an image is prepared for a class of use. The implementation can require different arrangements or metadata for rendering, sampling, transfer, or presentation.

This program records:

```text
UNDEFINED → COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
                write pixels             present
```

`UNDEFINED` at the start deliberately discards old contents. It does not claim the image has never been used. This is appropriate because every frame clears the attachment. If you later preserve pixels using a load operation, track and transition from the actual prior layout instead.

Setting `RenderingAttachmentInfo.imageLayout` tells Vulkan which layout the attachment will be in; it does not perform the transition. `pipelineBarrier2()` records the transition.

### Read the barriers in plain language

The upload barrier says: make the copy's buffer writes available to the later vertex/index input reads.

The initial image barrier says: after acquisition's dependency, prepare the image for color-attachment writes. Its source and destination stages are color-attachment output, matching the submission's acquire wait stage.

The depth barrier prepares the shared depth image for early/late fragment tests. It orders reuse after previous depth writes and discards old contents because this frame clears depth. Its destination access allows both depth reads and writes.

The final color image barrier says: after color-attachment writes, transition the image for presentation. Its destination stage/access are left as none because the following presentation dependency is carried by the semaphore.

Synchronization2 puts stage and access masks together in each barrier. `DependencyInfo` holds the barriers; `submit2()` supplies command-buffer and semaphore submission information. These are the core Vulkan 1.3 forms used throughout this starter. The [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html) provide other producer/consumer patterns.

**Stage** identifies the kind of operation. **Access** identifies the memory read/write involved. Color attachment writes happen in the color-attachment output stage, not in the fragment shader stage, even though the fragment shader supplies the color.

## 13. Resizing, shader compilation, and lifetime

### Resizing

Swapchain images have a size chosen for the surface. A resize can make the existing swapchain out of date or suboptimal.

The context waits for a usable window size, waits for device work, releases the old color/depth images and swapchain image views/semaphores/swapchain, then creates new resources at the current extent. It increments the swapchain generation.

The renderer sees that generation and rebuilds its graphics pipeline against the current color/depth formats. This conservatively rebuilds even if only the size changed. Dynamic viewport/scissor would allow reuse when the format stays compatible; the current approach keeps recreation straightforward.

When minimized, the window can have a zero framebuffer size. The context waits for window events until a drawable size returns or the window closes.

### Shader compilation

```text
mesh.vert / mesh.frag (GLSL source)
    ↓ glslc, invoked by CMake
mesh.vert.spv / mesh.frag.spv (SPIR-V binaries)
    ↓ loadModule()
Vulkan shader modules → graphics pipeline
```

The GPU does not run the `.vert` file directly. Edit GLSL source, then rebuild and restart using `./run.sh`. There is no shader hot reload.

### Resource lifetime

Anything referenced by submitted work must remain valid until the GPU is done with it. Examples include mesh buffers, descriptor resources, image views, and pipelines.

The startup upload waits before freeing staging memory. Swapchain recreation waits before releasing targets. Shutdown waits before destroying renderer resources. RAII then handles destruction order.

Validation catches many API and synchronization errors. It does not prove your scene looks right or your mathematics are correct.

## 14. Learn by changing one thing at a time

Rebuild with `./run.sh` after each edit. Predict the result before running it.

### Experiment 1: stop the rotation

In `updateScene()`, temporarily remove the line that adds to object 0's rotation.

Expected: both cubes remain still. Remove both rotation update lines. Shared elapsed time still changes, but the default shaders do not use that field.

This separates frame time, object animation, and shader behavior.

### Experiment 2: move one object

Change object 1's x position from `0.9f` to `0.5f` in `createScene()`.

Expected: only the right object moves left. Both objects still share the same mesh buffer.

### Experiment 3: replace the fragment output

In `mesh.frag`, replace the output assignment with:

```glsl
outColor = vec4(1.0, 0.0, 0.0, 1.0);
```

Expected: both cubes become solid red. Their geometry, transforms, and vertex colors still exist; the fragment shader simply ignores the interpolated color.

Restore the original assignment afterward.

### Experiment 4: use shared time in the fragment shader

The shader already has access to `frame.elapsedTime`:

```glsl
float brightness = 0.5 + 0.5 * sin(frame.elapsedTime);
outColor = vec4(color * brightness, 1.0);
```

Expected: both cubes pulse together, while only the first rotates. Time is shared per frame; rotation is per object.

### Experiment 5: add a third object

In `createScene()`, after creating the original objects:

```cpp
scene.objects.push_back({0, {{0, 1.3f, -1}, {0.4f, 0.4f, 0.4f}, {0, 0, 0}}});
```

Expected: another cube appears above the center and farther from the camera. It adds a draw and transform, not another mesh upload.

### Experiment 6: make a rectangle

Use the rectangle mesh example in [README.md](README.md). Leave the object transforms unchanged.

Expected: both objects become rectangles because they reference the same mesh. Indices determine the triangles that form each rectangle.

### Experiment 7: resize the window

Expected: rendering continues and the cubes retain their proportions because projection uses the current aspect ratio. Check validation output while resizing and minimizing/restoring.

This tests swapchain recreation separately from animation and mesh updates.

## 15. Where to look when something breaks

| Symptom | First checks |
| --- | --- |
| Background only | Object positions/scales, indices, clip position, bound pipeline/buffers |
| Unexpected geometry or colors | Vertex stride, offsets, formats, matching shader input locations |
| All objects have the same transform | Push the transform before each object's draw |
| Changing CPU vertices does nothing | The mesh is uploaded once; restart or implement an upload |
| Alpha does nothing | Blending is disabled |
| Far surfaces appear over near ones | Depth clear value, depth comparison, depth writes, projection Z range |
| Shader edit does nothing | Rebuild `.spv` and restart; no hot reload |
| Flicker or validation synchronization errors | Buffer reuse, acquisition wait, barriers, presentation semaphore reuse |
| Resize fails | Out-of-date handling, attachment format, recreated views, viewport/scissor |
| “GLFW needs a desktop display session” | Run from a graphical session that the process can access |

Run the existing checks after building:

```sh
ctest --test-dir build --output-on-failure
bash tests/render_smoke.sh
```

The first checks CPU scene behavior and MVP mathematics, including transform order, camera orientation, near/far depth, Y correction, aspect ratio, and perspective division. The second requires a display, validation layers, and synchronization validation; it renders 120 frames and fails on reported Vulkan warnings/errors. It does not inspect the image or automate resize/minimize behavior.

## 16. Code reading map and next topics

Read these in this order rather than starting at every device-creation structure:

1. [scene.hpp](src/scene.hpp): what data exists?
2. [scene.cpp](src/scene.cpp): what geometry and objects are created and animated?
3. [mesh.vert](shaders/mesh.vert): how does each vertex move?
4. [mesh.frag](shaders/mesh.frag): how does each fragment get its color?
5. [renderer.cpp](src/renderer.cpp), `recordSceneDraw()`: what does each draw bind?
6. Same file, `createPipeline()`: what rules interpret the vertex data and render it?
7. Same file, `createSceneResources()`: where do the bytes and descriptors come from?
8. [vulkan_context.cpp](src/vulkan_context.cpp), `beginFrame()` / `endFrame()`: how is one image acquired, rendered, and presented?
9. Same file, `uploadBuffer()`: how do static meshes reach device memory?
10. Same file, setup and recreation: how are the required Vulkan objects created and kept valid?

Once you can explain that path, useful next steps are interactive camera controls, then texture coordinates/images/samplers, then lighting with normals. The camera matrices and depth testing are already implemented. Multiple frames in flight and allocation optimizations can follow when you need them.

Compute is another GPU pipeline: you dispatch groups of shader invocations to read/write buffers or images, without the triangle rasterization stages. Compute-generated data can feed graphics, but needs the corresponding memory dependencies. This starter has no compute simulation yet.

Tessellation, geometry/mesh shaders, ray tracing, advanced descriptor schemes, and render graphs are outside this walkthrough. The goal here is to understand every major job in this starter well enough to predict what a small edit will do.
