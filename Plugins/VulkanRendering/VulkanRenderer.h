/******************************************************************************
Class:VulkanRenderer
Implements:RendererBase
Author:Rich Davison
Description:TODO

-_-_-_-_-_-_-_,------,
_-_-_-_-_-_-_-|   /\_/\   NYANYANYAN
-_-_-_-_-_-_-~|__( ^ .^) /
_-_-_-_-_-_-_-""  ""

*/
/////////////////////////////////////////////////////////////////////////////
#pragma once
#include "VulkanDef.h"

#include "RHI/RendererBase.h"

#ifdef _WIN32
    #define VK_USE_PLATFORM_WIN32_KHR
    //#include <Windows.h> //vulkan hpp needs this included beforehand!
    //#include <minwindef.h>
#endif

// 使用 C++20 指定初始化器
#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#include <vulkan/vulkan.hpp> //

#include "VulkanShader.h"
#include "VulkanMesh.h"
#include "VulkanShader.h"
#include "VulkanTexture.h"

#include "Window.h"

#include <vector>
#include <string>

using std::string;

namespace NLS
{
namespace Rendering
{
struct UniformData
{
    vk::Buffer buffer;
    vk::MemoryAllocateInfo allocInfo;
    vk::DeviceMemory deviceMem;
    vk::DescriptorBufferInfo descriptorInfo;
};

struct VulkanPipeline
{
    vk::Pipeline pipeline;
    vk::PipelineLayout layout;
};

class VULKAN_API VulkanRenderer : public RendererBase
{
    friend class VulkanMesh;
    friend class VulkanTexture;
    friend class VulkanPipelineBuilder;
    friend class VulkanShaderBuilder;
    friend class VulkanDescriptorSetLayoutBuilder;
    friend class VulkanRenderPassBuilder;

public:
    VulkanRenderer(Window& window);
    ~VulkanRenderer();

    void OnWindowResize(int w, int h) override;

    virtual ShaderBase* CreateShader(const std::string& vertex, const std::string& fragment) override;

    virtual void DrawString(const std::string& text, const Maths::Vector2& pos, const Maths::Vector4& colour, float size) override;
    virtual void DrawLine(const Maths::Vector3& start, const Maths::Vector3& end, const Maths::Vector4& colour) override;

protected:
    void BeginFrame() override;
    void EndFrame() override;
    void SwapBuffers() override;

    void SubmitDrawCall(VulkanMesh* m, vk::CommandBuffer& to);

    void SetDebugName(vk::ObjectType t, uint64_t handle, const string& debugName);

    virtual void CompleteResize();

    vk::DescriptorSet BuildDescriptorSet(vk::DescriptorSetLayout& layout);

    bool InitVulkan();

    bool InitInstance();
    bool InitDebugMessager();
    bool InitPhysicalDevice();
    bool InitLogicalDevice();

    int InitSwapChain();
    vk::Extent2D ChooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities);

    bool CreateDefaultFrameBuffers();

    bool InitSurface();

    bool InitDeviceQueueFamilies();

    void ImageTransitionBarrier(vk::CommandBuffer* buffer, vk::Image i, vk::ImageLayout oldLayout, vk::ImageLayout newLayout, vk::ImageAspectFlags aspect, vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage, int mipLevel = 0, int layer = 0);
    void ImageTransitionBarrier(vk::CommandBuffer* buffer, VulkanTexture* t, vk::ImageLayout oldLayout, vk::ImageLayout newLayout, vk::ImageAspectFlags aspect, vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage, int mipLevel = 0, int layer = 0);

    void CreateGraphicsPipeline();

    virtual void InitDefaultRenderPass();
    virtual void InitDefaultDescriptorPool();

    vk::CommandBuffer BeginCmdBuffer();
    void EndCmdBufferWait(vk::CommandBuffer& buffer);
    vk::Fence EndCmdBuffer(vk::CommandBuffer& buffer);
    void DestroyCmdBuffer(vk::CommandBuffer& buffer);

    void BeginSetupCmdBuffer();
    void EndSetupCmdBuffer();

    void InitCommandPool();
    void InitCommandBuffer();

    void PresentScreenImage();

    void InitUniformBuffer(UniformData& uniform, void* data, int dataSize);
    void UpdateUniformBuffer(UniformData& uniform, void* data, int dataSize);

    void UpdateImageDescriptor(vk::DescriptorSet& set, VulkanTexture* t, vk::Sampler sampler, int bindingNum = 0);
    void UpdateImageDescriptor(vk::DescriptorSet& set, VulkanTexture* t, vk::Sampler sampler, vk::ImageView, vk::ImageLayout forceLayout, int bindingNum = 0);

    vk::Device GetDevice() const
    {
        return device;
    }

    bool MemoryTypeFromPhysicalDeviceProps(vk::MemoryPropertyFlags requirements, uint32_t type, uint32_t& index);

    struct SwapChain
    {
        vk::Image image;
        vk::ImageView view;
    };

    struct QueueFamilyIndices
    {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;

        bool IsComplete()
        {
            return graphicsFamily.has_value() && presentFamily.has_value();
        }
    };

    vk::DispatchLoaderDynamic* dispatcher = nullptr;
    vk::DebugUtilsMessengerEXT debugMessenger;

    vk::SurfaceKHR surface;
    vk::Format surfaceFormat;
    vk::ColorSpaceKHR surfaceSpace;

    vk::Framebuffer* frameBuffers = nullptr;

    uint32_t numFrameBuffers = 0;
    VulkanTexture* depthBuffer = nullptr;

    vk::SwapchainKHR swapChain;
    vk::Extent2D swapChainExtent;

    std::vector<SwapChain*> swapChainList;
    uint32_t currentSwap = -1;

    vk::Instance instance;  // API Instance
    vk::PhysicalDevice gpu; // GPU in use
    vk::Device device;      // Device handle

    vk::PhysicalDeviceProperties deviceProperties;
    vk::PhysicalDeviceMemoryProperties deviceMemoryProperties;

    VulkanPipeline graphicsPipeline;
    vk::PipelineCache pipelineCache;
    vk::DescriptorPool defaultDescriptorPool; // descriptor sets come from here!
    vk::CommandPool commandPool;              // Source Command Buffers from here

    vk::CommandBuffer setupCmdBuffer;
    vk::CommandBuffer frameCmdBuffer;

    vk::RenderPass defaultRenderPass;
    vk::RenderPassBeginInfo defaultBeginInfo;

    vk::ClearValue defaultClearValues[2];
    vk::Viewport defaultViewport;
    vk::Rect2D defaultScissor;

    vk::Queue graphicsQueue;
    vk::Queue presentQueue;
    QueueFamilyIndices queueFamilyIndices;
};
} // namespace Rendering
} // namespace NLS