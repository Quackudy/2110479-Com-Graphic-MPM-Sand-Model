#pragma once
#include <array>
#include <atomic>
#include <filesystem>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

struct GLFWwindow;

// SETUP: buffer dies before its backing memory (members destroy in reverse order).
struct Buffer {
    vk::raii::DeviceMemory memory = nullptr;
    vk::raii::Buffer handle = nullptr;
};

// SETUP ONLY: device, allocation/upload, render targets, and frame synchronization.
// The application's GLFW window must outlive this context.
class VulkanContext {
  public:
    explicit VulkanContext(GLFWwindow *window = nullptr, bool compute = false, bool uncapped = false);
    ~VulkanContext();
    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;
    Buffer createBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                        vk::MemoryPropertyFlags properties);
    void writeBuffer(Buffer &buffer, const void *data, vk::DeviceSize size);
    bool beginFrame(const std::array<float, 4> &background);
    void endFrame();
    void beginRendering(const std::array<float, 4> &background);
    void beginCommands();
    void submitAndWait();
    void readBuffer(const Buffer &buffer, void *data, vk::DeviceSize size);
    vk::raii::ShaderModule loadShader(const std::filesystem::path &path);
    vk::PhysicalDeviceProperties properties() const { return gpu.getProperties(); }
    uint32_t timestampBits() const {
        return gpu.getQueueFamilyProperties()[queueFamily].timestampValidBits;
    }
    uint32_t validationMessages() const { return validationCount.load(); }
    bool validationEnabled() const { return validation; }
    bool syncValidationEnabled() const { return syncValidation; }
    std::atomic<uint32_t> validationCount{0};

    vk::raii::Device &device() { return device_; }
    vk::raii::CommandBuffer &command() { return command_; }
    const vk::Extent2D &extent() const { return extent_; }
    vk::Format colorFormat() const { return format; }
    vk::Format depthFormat() const { return depthFormat_; }
    vk::SampleCountFlagBits sampleCount() const { return samples_; }
    uint64_t swapchainGeneration() const { return swapchainGeneration_; }

  private:
    GLFWwindow *window;
    bool compute;
    bool uncapped;
    vk::raii::Context context;
    vk::raii::Instance instance = nullptr;
    vk::raii::DebugUtilsMessengerEXT messenger = nullptr;
    bool validation = false;
    bool syncValidation = false;
    vk::raii::SurfaceKHR surface = nullptr;
    vk::raii::PhysicalDevice gpu = nullptr;
    vk::raii::Device device_ = nullptr;
    vk::raii::Queue queue = nullptr;
    uint32_t queueFamily = 0;
    vk::raii::CommandPool commandPool = nullptr;
    vk::raii::CommandBuffer command_ = nullptr;
    vk::raii::Semaphore imageReady = nullptr;
    vk::raii::Fence frameFence = nullptr;
    vk::raii::SwapchainKHR swapchain = nullptr;
    vk::Extent2D extent_;
    vk::Format format = vk::Format::eUndefined;
    uint64_t swapchainGeneration_ = 0;
    std::vector<vk::Image> images;
    std::vector<vk::raii::ImageView> views;
    std::vector<vk::raii::Semaphore> renderDone;
    vk::Format depthFormat_ = vk::Format::eUndefined;
    vk::raii::DeviceMemory depthMemory = nullptr;
    vk::raii::Image depthImage = nullptr;
    vk::raii::ImageView depthView = nullptr;
    vk::SampleCountFlagBits samples_ = vk::SampleCountFlagBits::e1;
    vk::raii::DeviceMemory colorMemory = nullptr;
    vk::raii::Image colorImage = nullptr;
    vk::raii::ImageView colorView = nullptr;
    uint32_t imageIndex = 0;
    bool acquireSuboptimal = false;

    void createInstance();
    void setupDebugMessenger();
    void createSurface();
    void pickDevice();
    void createDevice();
    void createCommandResources();
    void createSyncObjects();
    uint32_t findMemoryType(uint32_t types, vk::MemoryPropertyFlags properties);
    void createSwapchainResources();
    void createSwapchain();
    void createImageViews();
    void createAttachmentResources();
    void destroySwapchain();
    bool recreateSwapchain();
};
