#version 450
// EDIT: fragment colors/effects. Shared elapsedTime is available for animation.
layout(location = 0) in vec3 color;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0, std140) uniform FrameUniforms {
    mat4 view;
    mat4 projection;
    float elapsedTime;
} frame;

void main() {
    outColor = vec4(color, 1.0);
}
