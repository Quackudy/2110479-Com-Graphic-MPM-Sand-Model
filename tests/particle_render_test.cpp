#include "renderer.hpp"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <iostream>
#include <stdexcept>
int main() {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    auto window = glfwCreateWindow(640, 480, "Particle playback smoke", nullptr, nullptr);
    if (!window) return 1;
    {
        VulkanContext context(window);
        if (!context.validationEnabled()) throw std::runtime_error("Vulkan validation layers unavailable");
        Renderer renderer(context, PROJECT_BINARY_DIRECTORY, 2, .01f, .03f);
        scene::Camera camera;
        camera.position = {1.1f,.7f,1.3f};
        camera.target = {.5f,.3f,.5f};
        const std::vector<float> a{.4f,.3f,.5f, .6f,.3f,.5f};
        const std::vector<float> b{.45f,.4f,.5f, .55f,.4f,.5f};
        for (uint32_t frame=0;frame<120;++frame) {
            glfwPollEvents();
            if (!context.beginFrame({.035f,.045f,.065f,1})) throw std::runtime_error("frame unavailable");
            renderer.setFrames(a,b);
            context.beginRendering({.035f,.045f,.065f,1});
            renderer.draw(camera,float(frame%60)/60);
            context.endFrame();
        }
        context.device().waitIdle();
        if (context.validationMessages()) throw std::runtime_error("particle playback validation errors");
        std::cout << "Processed 120 frame(s).\n";
    }
    glfwDestroyWindow(window);
    glfwTerminate();
}
