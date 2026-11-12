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
    auto clip = projection * scene::viewMatrix(camera) * glm::vec4(0, 0, 0, 1);
    require(clip.w > 0 && clip.z >= 0 && clip.z <= clip.w,
        "default MVP must place the origin inside the visible depth range");
    expectInvalid([&] { scene::projectionMatrix(camera, 0); });
    expectInvalid([&] { scene::projectionMatrix(camera, std::numeric_limits<float>::quiet_NaN()); });
    auto invalid = camera;
    invalid.target = invalid.position;
    expectInvalid([&] { scene::viewMatrix(invalid); });
    invalid = camera;
    invalid.up = {0,0,1};
    expectInvalid([&] { scene::viewMatrix(invalid); });
    invalid = camera;
    invalid.position.x = std::numeric_limits<float>::infinity();
    expectInvalid([&] { scene::viewMatrix(invalid); });
    for (float value : {0.f, std::numbers::pi_v<float>, std::numeric_limits<float>::quiet_NaN()}) {
        invalid = camera;
        invalid.verticalFov = value;
        expectInvalid([&] { scene::projectionMatrix(invalid,1); });
    }
    invalid = camera;
    invalid.nearPlane = 0;
    expectInvalid([&] { scene::projectionMatrix(invalid,1); });
    invalid = camera;
    invalid.farPlane = invalid.nearPlane;
    expectInvalid([&] { scene::projectionMatrix(invalid,1); });
    invalid.farPlane = std::numeric_limits<float>::infinity();
    expectInvalid([&] { scene::projectionMatrix(invalid,1); });
}
