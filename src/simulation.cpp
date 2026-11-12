#include "simulation.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

Simulation::Simulation(VulkanContext &context, const std::filesystem::path &shaders,
                       const SimulationConfig &config, const std::vector<Particle> &initial)
    : context(context), config(config),
      parameters(config.parameters(static_cast<uint32_t>(initial.size()), config.timestep())) {
    if (initial.empty())
        throw std::runtime_error("simulation has no particles");
    const auto gridBytes = vk::DeviceSize(parameters.dims.w) * sizeof(glm::vec4);
    if (gridBytes > context.properties().limits.maxStorageBufferRange ||
        initial.size() * sizeof(Particle) > context.properties().limits.maxStorageBufferRange)
        throw std::runtime_error("grid or particles exceed maxStorageBufferRange on this GPU; "
                                 "reduce grid/particle count");
    particles = context.createBuffer(initial.size() * sizeof(Particle),
                                     vk::BufferUsageFlagBits::eStorageBuffer |
                                         vk::BufferUsageFlagBits::eTransferDst |
                                         vk::BufferUsageFlagBits::eTransferSrc,
                                     vk::MemoryPropertyFlagBits::eDeviceLocal);
    grid = context.createBuffer(
        gridBytes, vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eDeviceLocal);
    reset(initial);
    healthBuffer = context.createBuffer(sizeof(Health), vk::BufferUsageFlagBits::eStorageBuffer,
                                        vk::MemoryPropertyFlagBits::eHostVisible |
                                            vk::MemoryPropertyFlagBits::eHostCoherent);
    healthMapped = healthBuffer.memory.mapMemory(0, sizeof(Health));
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t i = 0; i < 3; ++i)
        bindings[i] = {i, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute};
    vk::DescriptorSetLayoutCreateInfo di{};
    di.bindingCount = 3;
    di.pBindings = bindings.data();
    descriptors = vk::raii::DescriptorSetLayout(context.device(), di);
    vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer, 3};
    vk::DescriptorPoolCreateInfo pi{};
    pi.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    pool = vk::raii::DescriptorPool(context.device(), pi);
    auto raw = *descriptors;
    vk::DescriptorSetAllocateInfo ai{};
    ai.descriptorPool = *pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &raw;
    set = std::move(vk::raii::DescriptorSets(context.device(), ai).front());
    std::array<vk::DescriptorBufferInfo, 3> buffers{
        {{*particles.handle, 0, initial.size() * sizeof(Particle)},
         {*grid.handle, 0, gridBytes},
         {*healthBuffer.handle, 0, sizeof(Health)}}};
    std::array<vk::WriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].dstSet = *set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[i].pBufferInfo = &buffers[i];
    }
    context.device().updateDescriptorSets(writes, {});
    vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Parameters)};
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = 1;
    li.pSetLayouts = &raw;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    layout = vk::raii::PipelineLayout(context.device(), li);
    for (const auto *name : stageNames) {
        auto module = context.loadShader(shaders / (std::string(name) + ".comp.spv"));
        vk::ComputePipelineCreateInfo info{};
        info.stage = {{}, vk::ShaderStageFlagBits::eCompute, *module, "main"};
        info.layout = *layout;
        pipelines.emplace_back(context.device(), nullptr, info);
    }
    auto healthModule = context.loadShader(shaders / "particle_health.comp.spv");
    vk::ComputePipelineCreateInfo healthInfo{};
    healthInfo.stage = {{}, vk::ShaderStageFlagBits::eCompute, *healthModule, "main"};
    healthInfo.layout = *layout;
    healthPipeline = vk::raii::Pipeline(context.device(), nullptr, healthInfo);
    vk::QueryPoolCreateInfo qi{};
    qi.queryType = vk::QueryType::eTimestamp;
    qi.queryCount = 6 * 128;
    queries = vk::raii::QueryPool(context.device(), qi);
}
Simulation::~Simulation() {
    try {
        context.device().waitIdle();
        if (healthMapped)
            healthBuffer.memory.unmapMemory();
    } catch (const std::exception &e) {
        std::cerr << "Simulation shutdown: " << e.what() << '\n';
    }
}
void Simulation::reset(const std::vector<Particle> &initial) {
    if (initial.size() != parameters.dims.x)
        throw std::runtime_error("reset particle count differs");
    auto staging = context.createBuffer(
        initial.size() * sizeof(Particle), vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    context.writeBuffer(staging, initial.data(), initial.size() * sizeof(Particle));
    context.beginCommands();
    context.command().copyBuffer(*staging.handle, *particles.handle,
                                 vk::BufferCopy{0, 0, initial.size() * sizeof(Particle)});
    vk::MemoryBarrier2 mb{};
    mb.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
    mb.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    mb.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    mb.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    vk::DependencyInfo dep{};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    context.command().pipelineBarrier2(dep);
    context.submitAndWait();
    gpuMilliseconds.fill(0);
    substeps = 0;
}
void Simulation::barrier() {
    vk::MemoryBarrier2 mb{};
    mb.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    mb.srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    mb.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    mb.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    vk::DependencyInfo dep{};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    context.command().pipelineBarrier2(dep);
}
void Simulation::recordStage(Stage s) {
    auto &cmd = context.command();
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[s]);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0, *set, {});
    cmd.pushConstants<Parameters>(*layout, vk::ShaderStageFlagBits::eCompute, 0, parameters);
    const uint32_t count = (s == Clear || s == GridUpdate) ? parameters.dims.w : parameters.dims.x;
    cmd.dispatch((count + 127) / 128, 1, 1);
    barrier();
}
void Simulation::stage(Stage s, float dt) {
    parameters.step.x = dt;
    context.beginCommands();
    recordStage(s);
    context.submitAndWait();
}
void Simulation::advance(uint32_t steps, float dt) {
    parameters.step.x = dt;
    while (steps) {
        const uint32_t batch = std::min(steps, 128u), count = batch * 6;
        context.beginCommands();
        context.command().resetQueryPool(*queries, 0, count);
        for (uint32_t k = 0; k < batch; ++k) {
            context.command().writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *queries,
                                              6 * k);
            for (uint32_t s = 0; s < 5; ++s) {
                recordStage(static_cast<Stage>(s));
                context.command().writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands,
                                                  *queries, 6 * k + s + 1);
            }
        }
        context.submitAndWait();
        std::vector<uint64_t> times(count);
        auto result = vkGetQueryPoolResults(
            static_cast<VkDevice>(*context.device()), static_cast<VkQueryPool>(*queries), 0, count,
            times.size() * 8, times.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (result != VK_SUCCESS)
            throw std::runtime_error("cannot read GPU timestamps");
        const uint32_t bits = context.timestampBits();
        const uint64_t mask = bits == 64 ? UINT64_MAX : ((uint64_t(1) << bits) - 1);
        for (uint32_t k = 0; k < batch; ++k)
            for (uint32_t s = 0; s < 5; ++s)
                gpuMilliseconds[s] += double((times[6 * k + s + 1] - times[6 * k + s]) & mask) *
                                      context.properties().limits.timestampPeriod / 1e6;
        substeps += batch;
        steps -= batch;
    }
}
std::vector<Particle> Simulation::readParticles() {
    std::vector<Particle> result(parameters.dims.x);
    context.readBuffer(particles, result.data(), result.size() * sizeof(Particle));
    return result;
}
std::vector<glm::vec4> Simulation::readGrid() {
    std::vector<glm::vec4> result(parameters.dims.w);
    context.readBuffer(grid, result.data(), result.size() * sizeof(glm::vec4));
    return result;
}

Simulation::Health Simulation::health() {
    // Wait before touching mapped storage that the previous GPU dispatch may
    // still own.
    context.beginCommands();
    const Health initial{0, 0, UINT32_MAX, 0};
    std::memcpy(healthMapped, &initial, sizeof(initial));
    vk::MemoryBarrier2 mb{};
    mb.srcStageMask = vk::PipelineStageFlagBits2::eHost |
                      vk::PipelineStageFlagBits2::eComputeShader |
                      vk::PipelineStageFlagBits2::eCopy;
    mb.srcAccessMask = vk::AccessFlagBits2::eHostWrite | vk::AccessFlagBits2::eShaderWrite |
                       vk::AccessFlagBits2::eTransferWrite;
    mb.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    mb.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    vk::DependencyInfo dep{};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    context.command().pipelineBarrier2(dep);
    context.command().bindPipeline(vk::PipelineBindPoint::eCompute, *healthPipeline);
    context.command().bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0, *set, {});
    context.command().pushConstants<Parameters>(*layout, vk::ShaderStageFlagBits::eCompute, 0,
                                                parameters);
    context.command().dispatch((parameters.dims.x + 127) / 128, 1, 1);
    mb.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    mb.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
    mb.dstStageMask = vk::PipelineStageFlagBits2::eHost;
    mb.dstAccessMask = vk::AccessFlagBits2::eHostRead;
    context.command().pipelineBarrier2(dep);
    context.submitAndWait();
    Health result;
    std::memcpy(&result, healthMapped, sizeof(result));
    return result;
}
