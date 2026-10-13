#include "renderer.hpp"
#include <cstddef>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
std::vector<uint32_t> shaderCode(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open shader " + path.string());
    const auto bytes = input.tellg();
    if (bytes <= 0 || bytes % 4 != 0) throw std::runtime_error("invalid SPIR-V size");
    std::vector<uint32_t> words(static_cast<size_t>(bytes) / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(words.data()), bytes);
    if (!input) throw std::runtime_error("cannot read shader " + path.string());
    return words;
}

} // namespace

// Match the packed vertex and column-major model matrix layouts in mesh.vert.
static_assert(std::is_standard_layout_v<scene::Vertex>);
static_assert(sizeof(scene::Vertex) == 24);
static_assert(offsetof(scene::Vertex, color) == 12);
static_assert(sizeof(glm::mat4) == 64);

Renderer::Renderer(GLFWwindow* window, const std::filesystem::path& shaderDirectory, const scene::Scene& scene)
    : context(window), shaderDirectory(shaderDirectory) {
    scene::validateScene(scene);
    createSceneResources(scene);
    createPipeline();
}

Renderer::~Renderer() {
    // Wait before releasing meshes/descriptors/pipeline used by submitted commands.
    try { context.device().waitIdle(); }
    catch (const std::exception& error) { std::cerr << "Renderer shutdown: " << error.what() << '\n'; }
}

void Renderer::createSceneResources(const scene::Scene& scene) {
    // shortcut: one allocation per buffer, use VMA/suballocation for many meshes.
    meshes.reserve(scene.meshes.size());
    for (const auto& mesh : scene.meshes) {
        auto vertices = context.uploadBuffer(mesh.vertices.data(), sizeof(scene::Vertex) * mesh.vertices.size(),
            vk::BufferUsageFlagBits::eVertexBuffer);
        auto indices = context.uploadBuffer(mesh.indices.data(), sizeof(uint32_t) * mesh.indices.size(),
            vk::BufferUsageFlagBits::eIndexBuffer);
        meshes.push_back({std::move(vertices), std::move(indices), static_cast<uint32_t>(mesh.indices.size())});
    }
    uniformBuffer = context.createBuffer(sizeof(FrameUniforms), vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    vk::DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = vk::DescriptorType::eUniformBuffer;
    binding.descriptorCount = 1;
    binding.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    descriptorLayout = vk::raii::DescriptorSetLayout(context.device(), layoutInfo);
    vk::DescriptorPoolSize poolSize{vk::DescriptorType::eUniformBuffer, 1};
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    descriptorPool = vk::raii::DescriptorPool(context.device(), poolInfo);
    auto rawLayout = *descriptorLayout;
    vk::DescriptorSetAllocateInfo allocation{};
    allocation.descriptorPool = *descriptorPool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &rawLayout;
    descriptorSet = std::move(vk::raii::DescriptorSets(context.device(), allocation).front());
    vk::DescriptorBufferInfo bufferInfo{*uniformBuffer.handle, 0, sizeof(FrameUniforms)};
    vk::WriteDescriptorSet write{};
    write.dstSet = *descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eUniformBuffer;
    write.pBufferInfo = &bufferInfo;
    context.device().updateDescriptorSets(write, {});
}

vk::raii::ShaderModule Renderer::loadModule(const char* filename) {
    auto code = shaderCode(shaderDirectory / filename);
    vk::ShaderModuleCreateInfo info{};
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    return vk::raii::ShaderModule(context.device(), info);
}

void Renderer::createPipeline() {
    auto vert = loadModule("mesh.vert.spv");
    auto frag = loadModule("mesh.frag.spv");
    vk::PipelineShaderStageCreateInfo stages[2]{};
    stages[0].stage = vk::ShaderStageFlagBits::eVertex;
    stages[0].module = *vert; stages[0].pName = "main";
    stages[1].stage = vk::ShaderStageFlagBits::eFragment;
    stages[1].module = *frag; stages[1].pName = "main";
    // EDIT: if Vertex changes, keep these attributes and shader inputs in sync.
    vk::VertexInputBindingDescription vertexBinding{0, sizeof(scene::Vertex), vk::VertexInputRate::eVertex};
    std::array<vk::VertexInputAttributeDescription, 2> attributes{{
        {0, 0, vk::Format::eR32G32B32Sfloat, offsetof(scene::Vertex, position)},
        {1, 0, vk::Format::eR32G32B32Sfloat, offsetof(scene::Vertex, color)}
    }};
    vk::PipelineVertexInputStateCreateInfo input{};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &vertexBinding;
    input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    input.pVertexAttributeDescriptions = attributes.data();
    vk::PipelineInputAssemblyStateCreateInfo assembly{};
    // EDIT: drawing mode; scene indices currently describe triangle lists.
    assembly.topology = vk::PrimitiveTopology::eTriangleList;
    vk::PipelineViewportStateCreateInfo viewport{};
    viewport.viewportCount = viewport.scissorCount = 1;
    vk::PipelineRasterizationStateCreateInfo raster{};
    raster.polygonMode = vk::PolygonMode::eFill;
    raster.cullMode = vk::CullModeFlagBits::eBack;
    raster.frontFace = vk::FrontFace::eCounterClockwise;
    raster.lineWidth = 1.f;
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
    vk::DynamicState states[] = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic{};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    auto rawDescriptorLayout = *descriptorLayout;
    vk::PushConstantRange transformRange{vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4)};
    vk::PipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &transformRange;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &rawDescriptorLayout;
    pipeline = nullptr;
    layout = vk::raii::PipelineLayout(context.device(), layoutInfo);
    const auto colorFormat = context.colorFormat();
    vk::PipelineRenderingCreateInfo rendering{};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
    rendering.depthAttachmentFormat = context.depthFormat();
    vk::GraphicsPipelineCreateInfo info{};
    info.pNext = &rendering;
    info.stageCount = 2; info.pStages = stages;
    info.pVertexInputState = &input; info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport; info.pRasterizationState = &raster;
    info.pMultisampleState = &samples; info.pColorBlendState = &blend;
    info.pDepthStencilState = &depth;
    info.pDynamicState = &dynamic; info.layout = *layout;
    pipeline = vk::raii::Pipeline(context.device(), nullptr, info);
    pipelineGeneration = context.swapchainGeneration();
}

// EDIT DRAWING: one draw per object, with a separate transform for each draw.
void Renderer::recordSceneDraw(const scene::Scene& scene) {
    auto& command = context.command();
    command.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    command.setViewport(0, vk::Viewport{0, 0, float(context.extent().width), float(context.extent().height), 0, 1});
    command.setScissor(0, vk::Rect2D{{0, 0}, context.extent()});
    command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, *descriptorSet, {});
    for (const auto& object : scene.objects) {
        const auto& mesh = meshes[object.mesh];
        command.bindVertexBuffers(0, *mesh.vertices.handle, vk::DeviceSize{0});
        command.bindIndexBuffer(*mesh.indices.handle, 0, vk::IndexType::eUint32);
        const auto model = scene::modelMatrix(object.transform);
        command.pushConstants<glm::mat4>(*layout, vk::ShaderStageFlagBits::eVertex, 0, model);
        command.drawIndexed(mesh.indexCount, 1, 0, 0, 0);
    }
}

void Renderer::draw(const scene::Scene& scene) {
    if (scene.meshes.size() != meshes.size())
        throw std::runtime_error("mesh collection changed after upload; rebuild the renderer to upload new geometry");
    scene::validateObjects(scene);
    if (!context.beginFrame(scene.background)) return;
    if (pipelineGeneration != context.swapchainGeneration()) createPipeline();
    // beginFrame waits for the previous frame before we overwrite shared uniforms.
    const float aspect = float(context.extent().width) / float(context.extent().height);
    const FrameUniforms uniforms{scene::viewMatrix(scene.camera),
        scene::projectionMatrix(scene.camera, aspect), scene.elapsedTime};
    context.writeBuffer(uniformBuffer, &uniforms, sizeof(uniforms));
    recordSceneDraw(scene);
    context.endFrame();
}
