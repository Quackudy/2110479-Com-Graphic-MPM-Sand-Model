#include "scene.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <numbers>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace scene {
// EDIT GEOMETRY: local positions and colors. Every three indices make a triangle.
// Right-handed world: x right, y up, z toward the default camera. Upload once.
Mesh makeMesh() {
    return {
        {
            {{-0.5f, -0.5f,  0.5f}, {0.95f, 0.36f, 0.25f}},
            {{ 0.5f, -0.5f,  0.5f}, {0.29f, 0.78f, 0.49f}},
            {{ 0.5f,  0.5f,  0.5f}, {0.30f, 0.57f, 0.98f}},
            {{-0.5f,  0.5f,  0.5f}, {0.95f, 0.80f, 0.25f}},
            {{-0.5f, -0.5f, -0.5f}, {0.65f, 0.30f, 0.85f}},
            {{ 0.5f, -0.5f, -0.5f}, {0.25f, 0.80f, 0.85f}},
            {{ 0.5f,  0.5f, -0.5f}, {0.95f, 0.50f, 0.65f}},
            {{-0.5f,  0.5f, -0.5f}, {0.65f, 0.85f, 0.30f}},
        },
        {0, 1, 2, 2, 3, 0, 5, 4, 7, 7, 6, 5,
         4, 0, 3, 3, 7, 4, 1, 5, 6, 6, 2, 1,
         3, 2, 6, 6, 7, 3, 4, 5, 1, 1, 0, 4}
    };
}
// Column vectors: scale, rotate X/Y/Z, then translate (rightmost first).
glm::mat4 modelMatrix(const Transform& transform) {
    auto model = glm::translate(glm::mat4(1.0f), transform.position);
    model = glm::rotate(model, transform.rotation[2], glm::vec3(0, 0, 1));
    model = glm::rotate(model, transform.rotation[1], glm::vec3(0, 1, 0));
    model = glm::rotate(model, transform.rotation[0], glm::vec3(1, 0, 0));
    return glm::scale(model, transform.scale);
}

glm::mat4 viewMatrix(const Camera& camera) {
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

// EDIT SCENE: create meshes, then objects referring to those meshes.
Scene createScene() {
    Scene scene;
    scene.meshes.push_back(makeMesh());
    // Two objects share mesh 0, but have independent position/scale/rotation.
    scene.objects = {
        {0, {{-0.9f, 0.0f, 0.0f}, {0.9f, 0.9f, 0.9f}, {0.0f, 0.0f, 0.0f}}},
        {0, {{ 0.9f, 0.0f, 0.0f}, {0.9f, 0.9f, 0.9f}, {0.3f, 0.45f, 0.0f}}},
    };
    return scene;
}

// EDIT ANIMATION: change scene state, not GPU resources. deltaTime is seconds.
void updateScene(Scene& scene, float deltaTime) {
    if (!std::isfinite(deltaTime) || deltaTime < 0.0f)
        throw std::runtime_error("deltaTime must be finite and nonnegative");
    scene.elapsedTime += deltaTime;
    // Example: only the first object rotates. The second stays still.
    if (!scene.objects.empty()) {
        scene.objects[0].transform.rotation[0] += deltaTime * 0.5f;
        scene.objects[0].transform.rotation[1] += deltaTime;
    }
}

// SETUP: validate CPU data before GPU uploads/draws.
void validateMesh(const Mesh& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0)
        throw std::runtime_error("mesh needs vertices and complete index triangles");
    if (mesh.vertices.size() > std::numeric_limits<uint32_t>::max() ||
        mesh.indices.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("mesh exceeds 32-bit draw limits");
    for (auto index : mesh.indices)
        if (index >= mesh.vertices.size()) throw std::runtime_error("mesh index is outside vertex array");
    for (const auto& vertex : mesh.vertices) {
        for (float value : vertex.position)
            if (!std::isfinite(value)) throw std::runtime_error("mesh position must be finite");
        for (float value : vertex.color)
            if (!std::isfinite(value)) throw std::runtime_error("mesh color must be finite");
    }
}

void validateObjects(const Scene& scene) {
    const auto& camera = scene.camera;
    for (const auto& values : {camera.position, camera.target, camera.up})
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(values[axis])) throw std::runtime_error("camera vectors must be finite");
    const auto direction = camera.target - camera.position;
    const auto right = glm::cross(direction, camera.up);
    if (!std::isfinite(glm::dot(right, right)) || glm::dot(right, right) <= 1e-12f)
        throw std::runtime_error("camera needs distinct position/target and a nonparallel up vector");
    projectionMatrix(camera, 1.0f);
    if (!std::isfinite(scene.elapsedTime)) throw std::runtime_error("scene time must be finite");
    for (float value : scene.background)
        if (!std::isfinite(value)) throw std::runtime_error("background must be finite");
    for (const auto& object : scene.objects) {
        if (object.mesh >= scene.meshes.size()) throw std::runtime_error("object references a missing mesh");
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(object.transform.position[axis])) throw std::runtime_error("object position must be finite");
            if (!std::isfinite(object.transform.scale[axis])) throw std::runtime_error("object scale must be finite");
            if (!std::isfinite(object.transform.rotation[axis])) throw std::runtime_error("object rotation must be finite");
        }
    }
}

void validateScene(const Scene& scene) {
    for (const auto& mesh : scene.meshes) validateMesh(mesh);
    validateObjects(scene);
}
} // namespace scene
