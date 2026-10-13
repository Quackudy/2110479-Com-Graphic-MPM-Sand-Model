#pragma once
#include "scene.hpp"
#include "vulkan_context.hpp"
#include <filesystem>

// RENDERING: uploads meshes and draws scene objects; no application/animation loop.
class Renderer {
public:
    Renderer(GLFWwindow* window, const std::filesystem::path& shaderDirectory, const scene::Scene& scene);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void draw(const scene::Scene& scene);

private:
    VulkanContext context; // must outlive all renderer resources
    std::filesystem::path shaderDirectory;
    struct GpuMesh {
        Buffer vertices;
        Buffer indices;
        uint32_t indexCount;
    };
    // Match FrameUniforms in both shaders (std140).
    struct alignas(16) FrameUniforms {
        glm::mat4 view;
        glm::mat4 projection;
        float elapsedTime;
    };
    static_assert(offsetof(FrameUniforms, view) == 0);
    static_assert(offsetof(FrameUniforms, projection) == 64);
    static_assert(offsetof(FrameUniforms, elapsedTime) == 128);
    static_assert(sizeof(FrameUniforms) == 144);

    struct SDFUniforms {
        float sphere_pos;
        float sphere_size;
    };

    std::vector<GpuMesh> meshes;
    Buffer uniformBuffer;
    vk::raii::DescriptorSetLayout descriptorLayout = nullptr;
    vk::raii::DescriptorPool descriptorPool = nullptr;
    vk::raii::DescriptorSet descriptorSet = nullptr;
    vk::raii::PipelineLayout layout = nullptr;
    vk::raii::Pipeline pipeline = nullptr;
    uint64_t pipelineGeneration = 0;

    void createSceneResources(const scene::Scene& scene);
    vk::raii::ShaderModule loadModule(const char* filename);
    void createPipeline();
    void recordSceneDraw(const scene::Scene& scene);
};
