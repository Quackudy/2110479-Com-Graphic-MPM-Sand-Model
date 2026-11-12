#pragma once
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace scene {
struct Camera {
    glm::vec3 position{0.0f, 0.0f, 5.0f};
    glm::vec3 target{0.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float verticalFov = 1.04719755f; // 60 degrees in radians
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};
glm::mat4 viewMatrix(const Camera& camera);
glm::mat4 projectionMatrix(const Camera& camera, float aspect);
} // namespace scene
