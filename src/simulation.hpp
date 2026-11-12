#pragma once
#include "simulation_config.hpp"
#include "vulkan_context.hpp"
#include <array>

class Simulation {
  public:
    enum Stage { Clear = 0, P2G = 1, GridUpdate = 2, G2P = 3, Material = 4 };
    static constexpr std::array<const char *, 5> stageNames{"clear_grid", "p2g", "grid_update",
                                                            "g2p", "material_update"};
    Simulation(VulkanContext &context, const std::filesystem::path &shaders,
               const SimulationConfig &config, const std::vector<Particle> &particles);
    ~Simulation();
    void advance(uint32_t steps, float dt);
    void stage(Stage stage, float dt);
    std::vector<Particle> readParticles();
    std::vector<glm::vec4> readGrid();
    struct Health {
        float maxSpeed;
        uint32_t flags, firstBadParticle, padding;
    };
    static_assert(sizeof(Health) == 16);
    Health health();
    const Buffer &particleBuffer() const { return particles; }
    void reset(const std::vector<Particle> &particles);
    std::array<double, 5> gpuMilliseconds{};
    uint64_t substeps = 0;

  private:
    VulkanContext &context;
    SimulationConfig config;
    Parameters parameters;
    Buffer particles, grid, healthBuffer;
    void *healthMapped = nullptr;
    vk::raii::Pipeline healthPipeline = nullptr;
    vk::raii::DescriptorSetLayout descriptors = nullptr;
    vk::raii::DescriptorPool pool = nullptr;
    vk::raii::DescriptorSet set = nullptr;
    vk::raii::PipelineLayout layout = nullptr;
    std::vector<vk::raii::Pipeline> pipelines;
    vk::raii::QueryPool queries = nullptr;
    void recordStage(Stage stage);
    void barrier();
};
