#include "scene.hpp"
#include <cmath>
#include <stdexcept>
#include <numbers>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace scene {
glm::mat4 viewMatrix(const Camera& camera) {
    for (const auto& values : {camera.position, camera.target, camera.up})
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(values[axis])) throw std::runtime_error("camera vectors must be finite");
    const auto direction = camera.target - camera.position;
    const auto right = glm::cross(direction, camera.up);
    if (!std::isfinite(glm::dot(right, right)) || glm::dot(right, right) <= 1e-12f)
        throw std::runtime_error("camera needs distinct position/target and a nonparallel up vector");
    return glm::lookAtRH(camera.position, camera.target, camera.up);
}

glm::mat4 projectionMatrix(const Camera& camera, float aspect) {
    if (!std::isfinite(aspect) || aspect <= 0.0f ||
        !std::isfinite(camera.verticalFov) || camera.verticalFov <= 0.0f ||
        camera.verticalFov >= std::numbers::pi_v<float> ||
        !std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
        camera.nearPlane <= 0.0f || camera.farPlane <= camera.nearPlane)
        throw std::runtime_error("camera needs a valid perspective frustum and positive aspect ratio");
    auto projection = glm::perspectiveRH_ZO(camera.verticalFov, aspect, camera.nearPlane, camera.farPlane);
    projection[1][1] *= -1.0f; // Vulkan's positive-height viewport maps NDC y downward.
    return projection;
}

} // namespace scene
