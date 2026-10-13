#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace scene {
// EDIT: mesh layout. Keep renderer attributes and mesh.vert inputs in sync.
struct Vertex {
    std::array<float, 3> position;
    std::array<float, 3> color;
};
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};
struct Transform {
    // shortcut: Euler angles can gimbal lock, use quaternions for unrestricted orientation controls.
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f, 1.0f, 1.0f};
    glm::vec3 rotation{0.0f, 0.0f, 0.0f}; // x/y/z Euler angles, radians
};
struct Camera {
    glm::vec3 position{0.0f, 0.0f, 5.0f};
    glm::vec3 target{0.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float verticalFov = 1.04719755f; // 60 degrees in radians
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};
struct Object {
    size_t mesh = 0; // index into Scene::meshes; multiple objects may share a mesh
    Transform transform;
};
struct Scene {
    std::vector<Mesh> meshes;
    std::vector<Object> objects;
    Camera camera;
    std::array<float, 4> background{0.035f, 0.055f, 0.09f, 1.0f};
    float elapsedTime = 0.0f;
};

Mesh makeMesh();
glm::mat4 modelMatrix(const Transform& transform);
glm::mat4 viewMatrix(const Camera& camera);
glm::mat4 projectionMatrix(const Camera& camera, float aspect);
Scene createScene();
void updateScene(Scene& scene, float deltaTime);
void validateMesh(const Mesh& mesh);
void validateObjects(const Scene& scene);
void validateScene(const Scene& scene);
} // namespace scene
