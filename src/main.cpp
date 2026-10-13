#include "renderer.hpp"
#include <glm/detail/qualifier.hpp>
#include <glm/ext/vector_float3.hpp>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <chrono>
#include <glm/gtc/quaternion.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    
struct WindowPointerGLFW {
    scene::Scene* scene = nullptr;
};
    
double cursor_xpos{0}, cursor_ypos{0};

void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    auto* windowPointer = static_cast<WindowPointerGLFW*>(glfwGetWindowUserPointer(window));

    double prev_x = cursor_xpos;
    double prev_y = cursor_ypos;
    double delta_x = xpos - prev_x;
    double delta_y = ypos - prev_y;
    cursor_xpos = xpos;
    cursor_ypos = ypos;
    if (delta_x == 0.0 && delta_y == 0.0) return;
    
    int width{0}, height{0};
    glfwGetWindowSize(window, &width, &height);
    if (height == 0) return;

    glm::vec3 dir = windowPointer->scene->camera.target - windowPointer->scene->camera.position;

    glm::quat rotation_x = glm::angleAxis(
        glm::radians(static_cast<float>(delta_x) * 10 / height),
        glm::normalize(windowPointer->scene->camera.up));
    glm::vec3 yawed = rotation_x * dir;
    glm::quat rotation_y = glm::angleAxis(
        glm::radians(static_cast<float>(delta_y) * 10 / height),
        glm::normalize(glm::cross(yawed, windowPointer->scene->camera.up)));
    glm::vec3 rotated = rotation_y * yawed;



    windowPointer->scene->camera.target = windowPointer->scene->camera.position + rotated;
};



struct Window {
    GLFWwindow* handle = nullptr;
    ~Window() {
        if (handle) glfwDestroyWindow(handle);
        glfwTerminate();
    }
};

} // namespace

int main(int argc, char** argv) {
    try {
        uint32_t frames = 0;
        if (argc == 3 && std::string(argv[1]) == "--frames") {
            auto value = std::stoull(argv[2]);
            if (value > UINT32_MAX) throw std::runtime_error("frame count is too large");
            frames = static_cast<uint32_t>(value);
        } else if (argc != 1) {
            throw std::runtime_error("usage: ck_mpm_starter [--frames N]");
        }
        Window window;
        if (!glfwInit()) throw std::runtime_error("GLFW needs a desktop display session");
        if (!glfwVulkanSupported()) throw std::runtime_error("GLFW cannot find a Vulkan driver");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window.handle = glfwCreateWindow(960, 720, "Vulkan graphics starter", nullptr, nullptr);
        if (!window.handle) throw std::runtime_error("cannot create window");
        auto scene = scene::createScene();
        
        float deltaTime = 0.0f;
        
        
        WindowPointerGLFW windowPointer{.scene = &scene};
        glfwSetWindowUserPointer(window.handle, &windowPointer);
        glfwSetInputMode(window.handle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetCursorPos(window.handle, &cursor_xpos, &cursor_ypos);
        glfwSetCursorPosCallback(window.handle, cursor_position_callback);
        glfwSetKeyCallback(window.handle, [](GLFWwindow* w, int key, int, int action, int) {
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) glfwSetWindowShouldClose(w, GLFW_TRUE);
        });
        
        Renderer renderer(window.handle, std::filesystem::absolute(argv[0]).parent_path(), scene);
        auto previous = std::chrono::steady_clock::now();
        uint32_t rendered = 0;
        while (!glfwWindowShouldClose(window.handle) && (frames == 0 || rendered < frames)) {
            const auto now = std::chrono::steady_clock::now();
            deltaTime = std::chrono::duration<float>(now - previous).count();
            previous = now;
            glfwPollEvents();
            if (glfwGetKey(window.handle, GLFW_KEY_W) == GLFW_PRESS) {
                const glm::vec3 movement = glm::normalize(scene.camera.target - scene.camera.position) * (3.0f * deltaTime);
                scene.camera.position += movement;
                scene.camera.target += movement;
            }
            if (glfwGetKey(window.handle, GLFW_KEY_S) == GLFW_PRESS) {
                const glm::vec3 movement = glm::normalize(scene.camera.target - scene.camera.position) * (3.0f * deltaTime);
                scene.camera.position -= movement;
                scene.camera.target -= movement;
            }
            if (glfwGetKey(window.handle, GLFW_KEY_D) == GLFW_PRESS) {
                const glm::vec3 movement = glm::normalize(glm::cross(scene.camera.target - scene.camera.position, scene.camera.up)) * (3.0f * deltaTime);
                scene.camera.position += movement;
                scene.camera.target += movement;
            }
            if (glfwGetKey(window.handle, GLFW_KEY_A) == GLFW_PRESS) {
                const glm::vec3 movement = glm::normalize(glm::cross(scene.camera.target - scene.camera.position, scene.camera.up)) * (3.0f * deltaTime);
                scene.camera.position -= movement;
                scene.camera.target -= movement;
            }
            scene::updateScene(scene, deltaTime);
            renderer.draw(scene);
            ++rendered;
        }
        std::cout << "Processed " << rendered << " frame(s).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
