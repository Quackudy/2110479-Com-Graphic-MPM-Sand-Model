#include "scene.hpp"
#include <limits>
#include <stdexcept>
#include <cmath>
#include <numbers>
#include <glm/vec4.hpp>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void expectInvalid(Function function) {
    try { function(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("invalid input was accepted");
}

void requireNear(const glm::vec4& actual, const glm::vec4& expected, const char* message) {
    for (int i = 0; i < 4; ++i)
        require(std::isfinite(actual[i]) && std::abs(actual[i] - expected[i]) < 1e-4f, message);
}

int main() {
    const glm::vec4 point{1, 2, 3, 1};
    requireNear(scene::modelMatrix({}) * point, point, "default model is not identity");
    const float quarterTurn = std::numbers::pi_v<float> / 2.0f;
    scene::Transform transform{{3, 4, 5}, {2, 3, 4}, {quarterTurn, quarterTurn, quarterTurn}};
    requireNear(scene::modelMatrix(transform) * point, {15, 10, 3, 1},
        "model must scale, rotate X/Y/Z, then translate");
    scene::Camera camera;
    requireNear(scene::viewMatrix(camera) * glm::vec4(0, 0, 5, 1), {0, 0, 0, 1},
        "view must move the camera position to the origin");
    camera.position = {1, 2, 3};
    camera.target = {2, 2, 3};
    requireNear(scene::viewMatrix(camera) * glm::vec4(2, 2, 3, 1), {0, 0, -1, 1},
        "view must look down negative camera Z");
    requireNear(scene::viewMatrix(camera) * glm::vec4(1, 3, 3, 1), {0, 1, 0, 1},
        "view must preserve camera up");
    camera = {};
    auto projection = scene::projectionMatrix(camera, 1.0f);
    auto nearClip = projection * glm::vec4(0, 0, -camera.nearPlane, 1);
    auto farClip = projection * glm::vec4(0, 0, -camera.farPlane, 1);
    requireNear(nearClip / nearClip.w, {0, 0, 0, 1}, "Vulkan near depth must map to zero");
    requireNear(farClip / farClip.w, {0, 0, 1, 1}, "Vulkan far depth must map to one");
    require((projection * glm::vec4(0, 1, -2, 1)).y < 0, "projection must flip Vulkan Y");
    auto wideProjection = scene::projectionMatrix(camera, 2.0f);
    require(std::abs(wideProjection[0][0] * 2 - projection[0][0]) < 1e-5f,
        "projection must account for framebuffer aspect ratio");
    auto close = projection * glm::vec4(1, 0, -2, 1);
    auto distant = projection * glm::vec4(1, 0, -4, 1);
    require(std::abs(close.x / close.w - 2 * distant.x / distant.w) < 1e-5f,
        "distant positions must appear smaller after perspective division");
    auto clip = projection * scene::viewMatrix(camera) * scene::modelMatrix({}) * glm::vec4(0, 0, 0, 1);
    require(clip.w > 0 && clip.z >= 0 && clip.z <= clip.w,
        "default MVP must place the origin inside the visible depth range");
    expectInvalid([&] { scene::projectionMatrix(camera, 0); });
    expectInvalid([&] { scene::projectionMatrix(camera, std::numeric_limits<float>::quiet_NaN()); });
    auto scene = scene::createScene();
    scene::validateScene(scene);
    require(scene.objects.size() == 2, "demo needs two independently transformed objects");
    require(scene.objects[0].mesh == scene.objects[1].mesh, "demo objects must share geometry");
    const auto geometry = scene.meshes[0].vertices;
    const auto second = scene.objects[1].transform;
    scene::updateScene(scene, 0.25f);
    require(scene.elapsedTime == 0.25f, "scene time did not advance");
    require(scene.objects[0].transform.rotation[1] == 0.25f &&
            scene.objects[0].transform.rotation[0] == 0.125f, "first object did not rotate");
    require(scene.objects[1].transform.rotation == second.rotation &&
            scene.objects[1].transform.position == second.position &&
            scene.objects[1].transform.scale == second.scale, "updating one object affected the other");
    for (size_t i = 0; i < geometry.size(); ++i)
        require(scene.meshes[0].vertices[i].position == geometry[i].position, "object animation changed mesh geometry");
    auto split = scene::createScene();
    scene::updateScene(split, 0.125f);
    scene::updateScene(split, 0.125f);
    require(split.objects[0].transform.rotation == scene.objects[0].transform.rotation,
            "animation depends on frame count instead of deltaTime");
    expectInvalid([&] { scene::updateScene(scene, -1.0f); });
    expectInvalid([&] { scene::updateScene(scene, std::numeric_limits<float>::infinity()); });

    // A blank scene is valid; a referenced mesh must exist and contain triangles.
    scene::validateScene({});
    auto brokenScene = scene;
    brokenScene.objects[0].mesh = brokenScene.meshes.size();
    expectInvalid([&] { scene::validateScene(brokenScene); });
    brokenScene = scene;
    brokenScene.objects[0].transform.rotation[2] = std::numeric_limits<float>::quiet_NaN();
    expectInvalid([&] { scene::validateObjects(brokenScene); });
    brokenScene = scene;
    brokenScene.objects[0].transform.position[0] = std::numeric_limits<float>::infinity();
    expectInvalid([&] { scene::validateObjects(brokenScene); });
    brokenScene = scene;
    brokenScene.objects[0].transform.scale[1] = std::numeric_limits<float>::quiet_NaN();
    expectInvalid([&] { scene::validateObjects(brokenScene); });
    brokenScene = scene;
    brokenScene.background[0] = std::numeric_limits<float>::quiet_NaN();
    expectInvalid([&] { scene::validateObjects(brokenScene); });
    brokenScene = scene;
    brokenScene.elapsedTime = std::numeric_limits<float>::infinity();
    expectInvalid([&] { scene::validateObjects(brokenScene); });

    for (int axis = 0; axis < 3; ++axis) {
        brokenScene = scene;
        brokenScene.objects[0].transform.rotation[axis] = std::numeric_limits<float>::infinity();
        expectInvalid([&] { scene::validateObjects(brokenScene); });
    }
    brokenScene = scene;
    brokenScene.camera.target = brokenScene.camera.position;
    expectInvalid([&] { scene::validateObjects(brokenScene); });
    brokenScene = scene;
    brokenScene.camera.up = {0, 0, 1};
    expectInvalid([&] { scene::validateObjects(brokenScene); });
    brokenScene = scene;
    brokenScene.camera.position[2] = std::numeric_limits<float>::infinity();
    expectInvalid([&] { scene::validateObjects(brokenScene); });
    for (float invalid : {0.0f, std::numbers::pi_v<float>, std::numeric_limits<float>::quiet_NaN()}) {
        brokenScene = scene;
        brokenScene.camera.verticalFov = invalid;
        expectInvalid([&] { scene::validateObjects(brokenScene); });
    }
    brokenScene = scene;
    brokenScene.camera.nearPlane = 0;
    expectInvalid([&] { scene::validateObjects(brokenScene); });
    brokenScene = scene;
    brokenScene.camera.farPlane = brokenScene.camera.nearPlane;
    expectInvalid([&] { scene::validateObjects(brokenScene); });
    brokenScene = scene;
    brokenScene.camera.farPlane = std::numeric_limits<float>::infinity();
    expectInvalid([&] { scene::validateObjects(brokenScene); });

    const auto mesh = scene::makeMesh();
    require(mesh.vertices.size() == 8 && mesh.indices.size() == 36, "demo mesh must be a cube");
    // Shared vertices are valid: two triangles form a rectangle.
    scene::Mesh rectangle{
        {{{-0.5f, -0.5f, 0.0f}, {1, 0, 0}}, {{0.5f, -0.5f, 0.0f}, {0, 1, 0}},
         {{0.5f, 0.5f, 0.0f}, {0, 0, 1}}, {{-0.5f, 0.5f, 0.0f}, {1, 1, 0}}},
        {0, 1, 2, 2, 3, 0}
    };
    scene::validateMesh(rectangle);
    expectInvalid([] { scene::validateMesh({}); });
    auto broken = mesh;
    broken.indices.pop_back();
    expectInvalid([&] { scene::validateMesh(broken); });
    broken = mesh;
    broken.indices[0] = static_cast<uint32_t>(mesh.vertices.size());
    expectInvalid([&] { scene::validateMesh(broken); });
    broken = mesh;
    broken.vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    expectInvalid([&] { scene::validateMesh(broken); });
    broken = mesh;
    broken.vertices[0].color[0] = std::numeric_limits<float>::infinity();
    expectInvalid([&] { scene::validateMesh(broken); });
}
