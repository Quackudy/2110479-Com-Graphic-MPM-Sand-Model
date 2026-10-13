#include "vulkan_context.hpp"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
// Cap at 4x MSAA: a modest quality/cost balance for this starter.
constexpr vk::SampleCountFlagBits chooseSampleCount(vk::SampleCountFlags supported) {
    for (auto count : {vk::SampleCountFlagBits::e4, vk::SampleCountFlagBits::e2})
        if (supported & count) return count;
    return vk::SampleCountFlagBits::e1;
}
static_assert(chooseSampleCount(vk::SampleCountFlagBits::e1 | vk::SampleCountFlagBits::e2 |
    vk::SampleCountFlagBits::e4 | vk::SampleCountFlagBits::e8) == vk::SampleCountFlagBits::e4);
static_assert(chooseSampleCount(vk::SampleCountFlagBits::e1 | vk::SampleCountFlagBits::e2) == vk::SampleCountFlagBits::e2);
static_assert(chooseSampleCount(vk::SampleCountFlagBits::e1) == vk::SampleCountFlagBits::e1);

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT,
        vk::DebugUtilsMessageTypeFlagsEXT, const vk::DebugUtilsMessengerCallbackDataEXT* data, void*) {
    std::cerr << "[Vulkan validation] " << data->pMessage << '\n';
    return VK_FALSE;
}

template<typename Properties>
bool containsName(const std::vector<Properties>& properties, const char* name) {
    return std::any_of(properties.begin(), properties.end(), [name](const auto& p) {
        if constexpr (std::is_same_v<Properties, vk::LayerProperties>)
            return std::strcmp(p.layerName, name) == 0;
        else
            return std::strcmp(p.extensionName, name) == 0;
    });
}

} // namespace

VulkanContext::VulkanContext(GLFWwindow* window) : window(window) {
    createInstance();
    setupDebugMessenger();
    createSurface();
    pickDevice();
    createDevice();
    createCommandResources();
    createSyncObjects();
    createSwapchainResources();
    std::cout << "GPU: " << gpu.getProperties().deviceName.data()
              << " | validation: " << (validation ? "on" : "unavailable")
              << " | sync validation: " << (syncValidation ? "on" : "unavailable")
              << " | MSAA: " << static_cast<uint32_t>(samples_) << "x\n";
}

VulkanContext::~VulkanContext() {
    if (*device_) {
        try { device_.waitIdle(); }
        catch (const std::exception& error) { std::cerr << "Vulkan shutdown: " << error.what() << '\n'; }
    }
}

void VulkanContext::createInstance() {
    uint32_t glfwCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwCount);
    if (!glfwExtensions) throw std::runtime_error("GLFW has no Vulkan surface extensions");
    const auto layers = context.enumerateInstanceLayerProperties();
    const auto availableExtensions = context.enumerateInstanceExtensionProperties();
    validation = containsName(layers, "VK_LAYER_KHRONOS_validation") &&
        containsName(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    syncValidation = validation &&
        containsName(availableExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    if (!validation) std::cerr << "Validation layer unavailable; install Vulkan validation layers for development.\n";
    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwCount);
    if (validation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (syncValidation) extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    const char* layerName = "VK_LAYER_KHRONOS_validation";
    vk::ValidationFeatureEnableEXT sync = vk::ValidationFeatureEnableEXT::eSynchronizationValidation;
    vk::ValidationFeaturesEXT validationFeatures{};
    validationFeatures.enabledValidationFeatureCount = syncValidation ? 1u : 0u;
    validationFeatures.pEnabledValidationFeatures = &sync;
    vk::ApplicationInfo app{};
    app.pApplicationName = "Vulkan graphics starter";
    app.apiVersion = VK_API_VERSION_1_3;
    vk::InstanceCreateInfo instanceInfo{};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    instanceInfo.enabledLayerCount = validation ? 1u : 0u;
    instanceInfo.ppEnabledLayerNames = validation ? &layerName : nullptr;
    instanceInfo.pNext = syncValidation ? &validationFeatures : nullptr;
    instance = vk::raii::Instance(context, instanceInfo);
}

void VulkanContext::setupDebugMessenger() {
    if (validation) {
        vk::DebugUtilsMessengerCreateInfoEXT info{};
        info.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
        info.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
            vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
            vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
        info.pfnUserCallback = debugCallback;
        messenger = vk::raii::DebugUtilsMessengerEXT(instance, info);
    }
}

void VulkanContext::createSurface() {
    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(*instance, window, nullptr, &rawSurface) != VK_SUCCESS)
        throw std::runtime_error("cannot create window surface");
    surface = vk::raii::SurfaceKHR(instance, rawSurface);
}

void VulkanContext::createDevice() {
    float priority = 1.f;
    vk::DeviceQueueCreateInfo queueInfo{};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    vk::PhysicalDeviceVulkan13Features features{};
    features.dynamicRendering = vk::True;
    features.synchronization2 = vk::True;
    vk::DeviceCreateInfo deviceInfo{};
    deviceInfo.pNext = &features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = &swapchainExtension;
    device_ = vk::raii::Device(gpu, deviceInfo);
    queue = vk::raii::Queue(device_, queueFamily, 0);
}

void VulkanContext::createCommandResources() {
    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = queueFamily;
    commandPool = vk::raii::CommandPool(device_, poolInfo);
    vk::CommandBufferAllocateInfo allocation{};
    allocation.commandPool = *commandPool;
    allocation.level = vk::CommandBufferLevel::ePrimary;
    allocation.commandBufferCount = 1;
    command_ = std::move(vk::raii::CommandBuffers(device_, allocation).front());
}

void VulkanContext::createSyncObjects() {
    imageReady = vk::raii::Semaphore(device_, vk::SemaphoreCreateInfo{});
    vk::FenceCreateInfo fenceInfo{};
    fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
    frameFence = vk::raii::Fence(device_, fenceInfo);
}

uint32_t VulkanContext::findMemoryType(uint32_t types, vk::MemoryPropertyFlags properties) {
    const auto memory = gpu.getMemoryProperties();
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((types & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    throw std::runtime_error("no compatible GPU memory type");
}

Buffer VulkanContext::createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                    vk::MemoryPropertyFlags properties) {
    Buffer result;
    vk::BufferCreateInfo info{};
    info.size = size;
    info.usage = usage;
    info.sharingMode = vk::SharingMode::eExclusive;
    result.handle = vk::raii::Buffer(device_, info);
    const auto requirements = result.handle.getMemoryRequirements();
    vk::MemoryAllocateInfo allocation{};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties);
    result.memory = vk::raii::DeviceMemory(device_, allocation);
    result.handle.bindMemory(*result.memory, 0);
    return result;
}

void VulkanContext::writeBuffer(Buffer& buffer, const void* data, vk::DeviceSize size) {
    void* mapped = buffer.memory.mapMemory(0, size);
    std::memcpy(mapped, data, static_cast<size_t>(size));
    buffer.memory.unmapMemory();
}

Buffer VulkanContext::uploadBuffer(const void* data, vk::DeviceSize size, vk::BufferUsageFlagBits usage) {
    auto staging = createBuffer(size, vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    writeBuffer(staging, data, size);
    auto destination = createBuffer(size, usage | vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal);
    command_.reset();
    vk::CommandBufferBeginInfo begin{};
    begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    command_.begin(begin);
    command_.copyBuffer(*staging.handle, *destination.handle, vk::BufferCopy{0, 0, size});
    vk::BufferMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = usage == vk::BufferUsageFlagBits::eIndexBuffer
        ? vk::PipelineStageFlagBits2::eIndexInput : vk::PipelineStageFlagBits2::eVertexAttributeInput;
    barrier.dstAccessMask = usage == vk::BufferUsageFlagBits::eIndexBuffer
        ? vk::AccessFlagBits2::eIndexRead : vk::AccessFlagBits2::eVertexAttributeRead;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = *destination.handle;
    barrier.size = size;
    vk::DependencyInfo dependency{};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    command_.pipelineBarrier2(dependency);
    command_.end();
    vk::CommandBufferSubmitInfo commandInfo{};
    commandInfo.commandBuffer = *command_;
    vk::SubmitInfo2 submit{};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandInfo;
    queue.submit2(submit, nullptr);
    // Startup only: finish the copy before releasing staging memory.
    queue.waitIdle();
    return destination;
}

void VulkanContext::pickDevice() {
    auto candidates = instance.enumeratePhysicalDevices();
    int bestScore = -1;
    for (auto& candidate : candidates) {
        const auto props = candidate.getProperties();
        if (VK_API_VERSION_MAJOR(props.apiVersion) < 1 ||
            (VK_API_VERSION_MAJOR(props.apiVersion) == 1 && VK_API_VERSION_MINOR(props.apiVersion) < 3)) continue;
        const auto features = candidate.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features>();
        const auto& vulkan13 = features.get<vk::PhysicalDeviceVulkan13Features>();
        if (!vulkan13.dynamicRendering || !vulkan13.synchronization2) continue;
        if (!containsName(candidate.enumerateDeviceExtensionProperties(), VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        if (candidate.getSurfaceFormatsKHR(*surface).empty() ||
            candidate.getSurfacePresentModesKHR(*surface).empty()) continue;
        const auto families = candidate.getQueueFamilyProperties();
        for (uint32_t i = 0; i < families.size(); ++i) {
            if (!(families[i].queueFlags & vk::QueueFlagBits::eGraphics) ||
                !candidate.getSurfaceSupportKHR(i, *surface)) continue;
            const int score = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ? 2 : 1;
            if (score > bestScore) { gpu = candidate; queueFamily = i; bestScore = score; }
        }
    }
    if (!*gpu) throw std::runtime_error("no Vulkan 1.3 graphics GPU with dynamic rendering and synchronization2 can present to this window");
}

void VulkanContext::createSwapchainResources() {
    createSwapchain();
    createImageViews();
    createAttachmentResources();
    ++swapchainGeneration_;
}

void VulkanContext::createSwapchain() {
    const auto caps = gpu.getSurfaceCapabilitiesKHR(*surface);
    auto formats = gpu.getSurfaceFormatsKHR(*surface);
    auto chosen = formats.front();
    if (chosen.format == vk::Format::eUndefined)
        chosen = {vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
    for (const auto& candidate : formats)
        if (candidate.format == vk::Format::eB8G8R8A8Unorm &&
            candidate.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) chosen = candidate;
    format = chosen.format;
    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX) {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        extent_.width = std::clamp(static_cast<uint32_t>(width), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount) imageCount = std::min(imageCount, caps.maxImageCount);
    vk::SwapchainCreateInfoKHR info{};
    info.surface = *surface;
    info.minImageCount = imageCount;
    info.imageFormat = format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
    info.imageSharingMode = vk::SharingMode::eExclusive;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (!(caps.supportedCompositeAlpha & info.compositeAlpha)) {
        for (auto alpha : {vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
                           vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
                           vk::CompositeAlphaFlagBitsKHR::eInherit})
            if (caps.supportedCompositeAlpha & alpha) { info.compositeAlpha = alpha; break; }
    }
    info.presentMode = vk::PresentModeKHR::eFifo;
    info.clipped = vk::True;
    swapchain = vk::raii::SwapchainKHR(device_, info);
    images = swapchain.getImages();
}

void VulkanContext::createImageViews() {
    for (auto image : images) {
        vk::ImageViewCreateInfo view{};
        view.image = image;
        view.viewType = vk::ImageViewType::e2D;
        view.format = format;
        view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        views.emplace_back(device_, view);
        renderDone.emplace_back(device_, vk::SemaphoreCreateInfo{});
    }
}

void VulkanContext::destroySwapchain() {
    colorView = nullptr; colorImage = nullptr; colorMemory = nullptr;
    depthView = nullptr; depthImage = nullptr; depthMemory = nullptr;
    views.clear(); renderDone.clear(); images.clear();
    swapchain = nullptr;
}

void VulkanContext::createAttachmentResources() {
    depthFormat_ = vk::Format::eUndefined;
    for (auto candidate : {vk::Format::eD32Sfloat, vk::Format::eD16Unorm}) {
        if (gpu.getFormatProperties(candidate).optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment) {
            depthFormat_ = candidate;
            break;
        }
    }
    if (depthFormat_ == vk::Format::eUndefined)
        throw std::runtime_error("no supported depth attachment format");
    const auto limits = gpu.getProperties().limits;
    const auto colorSupport = gpu.getImageFormatProperties(format, vk::ImageType::e2D,
        vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eColorAttachment, {});
    const auto depthSupport = gpu.getImageFormatProperties(depthFormat_, vk::ImageType::e2D,
        vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eDepthStencilAttachment, {});
    samples_ = chooseSampleCount(limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts &
        colorSupport.sampleCounts & depthSupport.sampleCounts);

    auto createAttachment = [&](vk::Format attachmentFormat, vk::ImageUsageFlagBits usage,
            vk::ImageAspectFlagBits aspect, vk::raii::DeviceMemory& memory,
            vk::raii::Image& handle, vk::raii::ImageView& imageView) {
        vk::ImageCreateInfo image{};
        image.imageType = vk::ImageType::e2D;
        image.format = attachmentFormat;
        image.extent = vk::Extent3D{extent_.width, extent_.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = samples_;
        image.tiling = vk::ImageTiling::eOptimal;
        image.usage = usage;
        handle = vk::raii::Image(device_, image);
        const auto requirements = handle.getMemoryRequirements();
        vk::MemoryAllocateInfo allocation{};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        memory = vk::raii::DeviceMemory(device_, allocation);
        handle.bindMemory(*memory, 0);
        vk::ImageViewCreateInfo view{};
        view.image = *handle;
        view.viewType = vk::ImageViewType::e2D;
        view.format = attachmentFormat;
        view.subresourceRange = {aspect, 0, 1, 0, 1};
        imageView = vk::raii::ImageView(device_, view);
    };
    createAttachment(depthFormat_, vk::ImageUsageFlagBits::eDepthStencilAttachment,
        vk::ImageAspectFlagBits::eDepth, depthMemory, depthImage, depthView);
    if (samples_ != vk::SampleCountFlagBits::e1)
        createAttachment(format, vk::ImageUsageFlagBits::eColorAttachment,
            vk::ImageAspectFlagBits::eColor, colorMemory, colorImage, colorView);
}

bool VulkanContext::recreateSwapchain() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    while ((width == 0 || height == 0) && !glfwWindowShouldClose(window)) {
        glfwWaitEventsTimeout(0.1);
        glfwGetFramebufferSize(window, &width, &height);
    }
    if (glfwWindowShouldClose(window)) return false;
    device_.waitIdle();
    destroySwapchain();
    createSwapchainResources();
    return true;
}

// shortcut: one frame in flight, duplicate frame resources for concurrent rendering.
bool VulkanContext::beginFrame(const std::array<float, 4>& background) {
    if (device_.waitForFences(*frameFence, vk::True, UINT64_MAX) != vk::Result::eSuccess)
        throw std::runtime_error("cannot wait for frame fence");
    auto [acquired, acquiredIndex] = swapchain.acquireNextImage(UINT64_MAX, *imageReady, nullptr);
    if (acquired == vk::Result::eErrorOutOfDateKHR) { recreateSwapchain(); return false; }
    if (acquired != vk::Result::eSuccess && acquired != vk::Result::eSuboptimalKHR)
        throw std::runtime_error("cannot acquire swapchain image");
    imageIndex = acquiredIndex;
    acquireSuboptimal = acquired == vk::Result::eSuboptimalKHR;
    command_.reset();
    vk::CommandBufferBeginInfo begin{};
    begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    command_.begin(begin);
    vk::ImageMemoryBarrier2 barrier{};
    // Match the acquire semaphore's wait stage before touching the image.
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    barrier.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
    // Discard the previous frame's pixels: this rendering always clears the image.
    barrier.oldLayout = vk::ImageLayout::eUndefined;
    barrier.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[imageIndex];
    barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::ImageMemoryBarrier2 depthBarrier{};
    // One shared depth image: order reuse after the previous frame's depth writes.
    depthBarrier.srcStageMask = depthBarrier.dstStageMask =
        vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    depthBarrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depthBarrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
        vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depthBarrier.oldLayout = vk::ImageLayout::eUndefined;
    depthBarrier.newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    depthBarrier.srcQueueFamilyIndex = depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.image = *depthImage;
    depthBarrier.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    std::array barriers{barrier, depthBarrier, barrier};
    // The shared MSAA image is cleared and reused after previous color writes.
    if (samples_ != vk::SampleCountFlagBits::e1) {
        barriers[2].image = *colorImage;
        barriers[2].srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
        barriers[2].dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
            vk::AccessFlagBits2::eColorAttachmentWrite;
    }
    vk::DependencyInfo dependency{};
    dependency.imageMemoryBarrierCount = samples_ == vk::SampleCountFlagBits::e1 ? 2 : 3;
    dependency.pImageMemoryBarriers = barriers.data();
    command_.pipelineBarrier2(dependency);
    vk::RenderingAttachmentInfo color{};
    color.imageView = *views[imageIndex];
    color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color.loadOp = vk::AttachmentLoadOp::eClear;
    color.storeOp = vk::AttachmentStoreOp::eStore;
    color.clearValue.color = vk::ClearColorValue(background);
    if (samples_ != vk::SampleCountFlagBits::e1) {
        color.imageView = *colorView;
        color.resolveMode = vk::ResolveModeFlagBits::eAverage;
        color.resolveImageView = *views[imageIndex];
        color.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color.storeOp = vk::AttachmentStoreOp::eDontCare;
    }
    vk::RenderingAttachmentInfo depth{};
    depth.imageView = *depthView;
    depth.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    depth.loadOp = vk::AttachmentLoadOp::eClear;
    depth.storeOp = vk::AttachmentStoreOp::eDontCare;
    depth.clearValue.depthStencil = vk::ClearDepthStencilValue{1.0f, 0};
    vk::RenderingInfo rendering{};
    rendering.renderArea.extent = extent_;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    command_.beginRendering(rendering);
    return true;
}

void VulkanContext::endFrame() {
    command_.endRendering();
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    barrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
    barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
    barrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[imageIndex];
    barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::DependencyInfo dependency{};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    command_.pipelineBarrier2(dependency);
    command_.end();
    device_.resetFences(*frameFence);
    vk::SemaphoreSubmitInfo wait{};
    wait.semaphore = *imageReady;
    wait.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    vk::CommandBufferSubmitInfo commandInfo{};
    commandInfo.commandBuffer = *command_;
    vk::SemaphoreSubmitInfo signal{};
    signal.semaphore = *renderDone[imageIndex];
    signal.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
    auto rawRenderDone = *renderDone[imageIndex];
    vk::SubmitInfo2 submit{};
    submit.waitSemaphoreInfoCount = 1; submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &commandInfo;
    submit.signalSemaphoreInfoCount = 1; submit.pSignalSemaphoreInfos = &signal;
    queue.submit2(submit, *frameFence);
    auto rawSwapchain = *swapchain;
    vk::PresentInfoKHR present{};
    present.waitSemaphoreCount = 1; present.pWaitSemaphores = &rawRenderDone;
    present.swapchainCount = 1; present.pSwapchains = &rawSwapchain; present.pImageIndices = &imageIndex;
    auto result = queue.presentKHR(present);
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR ||
        acquireSuboptimal) recreateSwapchain();
    else if (result != vk::Result::eSuccess) throw std::runtime_error("cannot present swapchain image");
}
