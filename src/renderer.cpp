#include "renderer.hpp"
#include "simulation_config.hpp"
#include <iostream>
#include <stdexcept>

Renderer::Renderer(VulkanContext &context, const std::filesystem::path &shaders, uint32_t particles,
                   float radius, float floorHeight, const Buffer *liveParticles)
    : context(context), shaders(shaders), count(particles), radius(radius), floorHeight(floorHeight) {
    liveParticleMode = liveParticles != nullptr;
    const size_t currentBytes = count * (liveParticleMode ? sizeof(Particle) : 3 * sizeof(float));
    const size_t nextBytes = currentBytes;
    if (!currentBytes || !nextBytes ||
        currentBytes > context.properties().limits.maxStorageBufferRange ||
        nextBytes > context.properties().limits.maxStorageBufferRange)
        throw std::runtime_error("render data exceeds GPU storage buffer limits");
    currentCapacity = liveParticleMode ? 0 : currentBytes;
    nextCapacity = liveParticleMode ? 0 : nextBytes;
    auto host =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    uniformBuffer =
        context.createBuffer(sizeof(Frame), vk::BufferUsageFlagBits::eUniformBuffer, host);
    vk::BufferUsageFlags vertexUsage = vk::BufferUsageFlagBits::eStorageBuffer;
    if (!liveParticleMode) {
        current = context.createBuffer(currentBytes, vertexUsage, host);
        next = context.createBuffer(nextBytes, vk::BufferUsageFlagBits::eStorageBuffer, host);
    }
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{
        {{0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment},
         {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex},
         {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex}}};
    vk::DescriptorSetLayoutCreateInfo di{};
    di.bindingCount = 3;
    di.pBindings = bindings.data();
    descriptors = vk::raii::DescriptorSetLayout(context.device(), di);
    std::array<vk::DescriptorPoolSize, 2> sizes{
        {{vk::DescriptorType::eUniformBuffer, 1}, {vk::DescriptorType::eStorageBuffer, 2}}};
    vk::DescriptorPoolCreateInfo pi{};
    pi.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes.data();
    pool = vk::raii::DescriptorPool(context.device(), pi);
    auto raw = *descriptors;
    vk::DescriptorSetAllocateInfo ai{};
    ai.descriptorPool = *pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &raw;
    set = std::move(vk::raii::DescriptorSets(context.device(), ai).front());
    std::array<vk::DescriptorBufferInfo, 3> buffers{
        {{*uniformBuffer.handle, 0, sizeof(Frame)},
         {liveParticleMode ? *liveParticles->handle : *current.handle, 0, currentBytes},
         {liveParticleMode ? *liveParticles->handle : *next.handle, 0, nextBytes}}};
    std::array<vk::WriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].dstSet = *set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType;
        writes[i].pBufferInfo = &buffers[i];
    }
    context.device().updateDescriptorSets(writes, {});
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = 1;
    li.pSetLayouts = &raw;
    layout = vk::raii::PipelineLayout(context.device(), li);
    createPipelines();
}
Renderer::~Renderer() {
    try {
        context.device().waitIdle();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
    }
}
void Renderer::createPipelines() {
    pipeline = createPipeline();
    generation = context.swapchainGeneration();
}
vk::raii::Pipeline Renderer::createPipeline() {
    auto vert =
             context.loadShader(shaders / "particle.vert.spv"),
         frag = context.loadShader(shaders / "particle.frag.spv");
    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{
        {{{}, vk::ShaderStageFlagBits::eVertex, *vert, "main"},
         {{}, vk::ShaderStageFlagBits::eFragment, *frag, "main"}}};
    const uint32_t stride = liveParticleMode ? sizeof(Particle) / sizeof(float) : 3;
    vk::SpecializationMapEntry strideEntry{0, 0, sizeof(stride)};
    vk::SpecializationInfo specialization{1, &strideEntry, sizeof(stride), &stride};
    stages[0].pSpecializationInfo = &specialization;
    vk::PipelineVertexInputStateCreateInfo input{};
    vk::PipelineInputAssemblyStateCreateInfo assembly{};
    assembly.topology = vk::PrimitiveTopology::eTriangleList;
    vk::PipelineViewportStateCreateInfo viewport{};
    viewport.viewportCount = viewport.scissorCount = 1;
    vk::PipelineRasterizationStateCreateInfo raster{};
    raster.polygonMode = vk::PolygonMode::eFill;
    raster.cullMode = vk::CullModeFlagBits::eNone;
    raster.lineWidth = 1;
    vk::PipelineMultisampleStateCreateInfo samples{};
    samples.rasterizationSamples = context.sampleCount();
    vk::PipelineDepthStencilStateCreateInfo depth{};
    depth.depthTestEnable = depth.depthWriteEnable = vk::True;
    depth.depthCompareOp = vk::CompareOp::eLess;
    vk::PipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo blend{};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    std::array states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic{};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states.data();
    auto color = context.colorFormat();
    vk::PipelineRenderingCreateInfo rendering{};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color;
    rendering.depthAttachmentFormat = context.depthFormat();
    vk::GraphicsPipelineCreateInfo info{};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages.data();
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = *layout;
    return vk::raii::Pipeline(context.device(), nullptr, info);
}
void Renderer::setFrames(const std::vector<float> &a, const std::vector<float> &b) {
    if (liveParticleMode)
        throw std::runtime_error("renderer does not own particle playback buffers");
    if (a.size() != size_t(count) * 3 || b.size() != a.size())
        throw std::runtime_error("playback particle count differs");
    // The application begins the frame (and waits for its fence) before updating
    // buffers.
    context.writeBuffer(current, a.data(), a.size() * sizeof(float));
    context.writeBuffer(next, b.data(), b.size() * sizeof(float));
}
void Renderer::prepareFrame() {
    if (liveParticleMode) {
        vk::MemoryBarrier2 barrier{};
        barrier.srcStageMask =
            vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eCopy;
        barrier.srcAccessMask =
            vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eVertexShader;
        barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
        vk::DependencyInfo dependency{};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        context.command().pipelineBarrier2(dependency);
        return;
    }
}
void Renderer::draw(const scene::Camera &camera, float alpha) {
    if (generation != context.swapchainGeneration())
        createPipelines();
    Frame frame{scene::viewMatrix(camera),
                scene::projectionMatrix(camera, float(context.extent().width) / context.extent().height),
                {alpha, radius, float(count), floorHeight}};
    context.writeBuffer(uniformBuffer, &frame, sizeof(frame));
    auto &cmd = context.command();
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, *set, {});
    cmd.setViewport(
        0, vk::Viewport{0, 0, float(context.extent().width), float(context.extent().height), 0, 1});
    cmd.setScissor(0, vk::Rect2D{{0, 0}, context.extent()});
    cmd.draw(6, count + 1, 0, 0);
}
