#version 450
// EDIT: local -> model/world -> view/camera -> projection/clip.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 color;
layout(set = 0, binding = 0, std140) uniform FrameUniforms {
    mat4 view;
    mat4 projection;
    float elapsedTime;
} frame;
// One model matrix per draw; matches the C++ glm::mat4 push constant.
layout(push_constant, std430) uniform ObjectTransform {
    mat4 model;
} object;

void main() {
    gl_Position = frame.projection * frame.view * object.model * vec4(inPosition, 1.0);
    color = inColor;
}
