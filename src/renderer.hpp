#pragma once
#include "scene.hpp"
#include "vulkan_context.hpp"
#include <filesystem>

// Draw saved or GPU-resident particles; the application owns frame submission.
class Renderer {
  public:
    Renderer(VulkanContext &context, const std::filesystem::path &shaders, uint32_t particles,
             float radius, float floorHeight, const Buffer *liveParticles = nullptr);
    ~Renderer();
    void setFrames(const std::vector<float> &a, const std::vector<float> &b);
    void prepareFrame();
    size_t particleBufferBytes() const { return currentCapacity + nextCapacity; }
    void draw(const scene::Camera &camera, float alpha);

  private:
    VulkanContext &context;
    std::filesystem::path shaders;
    uint32_t count;
    float radius, floorHeight;
    bool liveParticleMode = false;
    struct alignas(16) Frame {
        glm::mat4 view, projection;
        glm::vec4 playback;
    };
    static_assert(sizeof(Frame) == 144);
    Buffer uniformBuffer, current, next;
    size_t currentCapacity = 0, nextCapacity = 0;
    vk::raii::DescriptorSetLayout descriptors = nullptr;
    vk::raii::DescriptorPool pool = nullptr;
    vk::raii::DescriptorSet set = nullptr;
    vk::raii::PipelineLayout layout = nullptr;
    vk::raii::Pipeline pipeline = nullptr;
    uint64_t generation = 0;
    void createPipelines();
    vk::raii::Pipeline createPipeline();
};
