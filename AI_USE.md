# AI-use log

| Date | Assistance | Accepted output | Human check still needed |
| --- | --- | --- | --- |
| 2026-10-08 | Drafted W1 Vulkan triangle starter with ChatGPT Codex | CMake, GLSL, C++ window/render loop, README | Run on the project laptop, inspect triangle and validation output, review Vulkan resource lifetime and explain each part before submission. |
| 2026-10-08 | Converted starter to Vulkan-Hpp RAII with ChatGPT Codex | Resource wrappers, Vulkan-Hpp calls, C++20 build | Run on the project laptop and verify swapchain recreation and validation output. |
| 2026-10-11 | Prepared editable graphics starter with ChatGPT Codex | Separate scene/renderer, staged vertex/index buffers, uniform descriptors, edit markers, README exercises, CPU validation test | Inspect rendering, animation, resize/minimize/restore and validation output on the project laptop. |
| 2026-10-11 | Refactored scene/object architecture with ChatGPT Codex | Application loop, independent object transforms with per-draw push constants, shared meshes, Vulkan context, updated tests and guide | Inspect two objects, independent animation, resize/minimize/restore, and validation output on the project laptop. |
| 2026-10-12 | Migrated rendering and wrote beginner walkthrough with ChatGPT Codex | Vulkan 1.3 dynamic rendering, synchronization2 barriers/submissions, feature checks, rendering smoke script, graphics/Vulkan walkthrough | Build and CPU tests passed; this environment could not access the desktop display. Run the rendering smoke check and inspect animation, resize/minimize/restore on the project laptop. |
| 2026-10-12 | Extended starter to 3D with ChatGPT Codex | XYZ transforms/vertices, shared cube mesh, GLM model/view/projection matrices, perspective camera, depth attachment/testing, math tests, updated walkthrough | Inspect cube rotation, occlusion, camera and resize/minimize/restore with Vulkan validation on the project laptop; desktop access remains unavailable in this environment. |
| 2026-10-12 | Added MSAA with ChatGPT Codex | 4× target with 2×/1× hardware fallback, matching color/depth sample counts, dynamic-rendering resolve, resize/lifetime handling, compile-time selection checks, updated docs/smoke script | Run rendering smoke check and inspect edge smoothing/resize with validation on the project laptop; desktop access remains unavailable here. |

Add a row for later assistance and corrections. No external visual assets are used.
