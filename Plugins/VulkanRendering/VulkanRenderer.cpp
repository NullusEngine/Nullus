#include "VulkanRenderer.h"
#include "VulkanMesh.h"
#include "VulkanTexture.h"

#include "TextureLoader.h"
#include "MeshLoader.h"
#include "VulkanShaderBuilder.h"
#include "VulkanPipelineBuilder.h"

#ifdef WIN32
    #include "WIn32/Win32Window.h"
using namespace NLS::Win32Code;
#endif
#include "Maths.h"

#include <set>

const bool enableValidationLayers = true;
const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation",
};
const std::vector<const char*> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

using namespace NLS;
using namespace Rendering;

VulkanRenderer::VulkanRenderer(Window& window)
    : RendererBase(window)
{
    InitVulkan();

    // window resize will create the swap chain and frame buffers
    VulkanTexture::SetRenderer(this);
    TextureLoader::RegisterAPILoadFunction(VulkanTexture::VulkanTextureFromFilename);
    TextureLoader::RegisterAPICubeMapLoadFunction([](const CubeMapFileNames& filenames)
                                                  { return VulkanTexture::VulkanCubemapFromFilename(filenames[0], filenames[1], filenames[2], filenames[3], filenames[4], filenames[5]); });
    MeshLoader::RegisterAPILoadFunction([](const std::string& filename)
                                        { return new VulkanMesh(filename); });

    OnWindowResize((int)hostWindow.GetScreenSize().x, (int)hostWindow.GetScreenSize().y);

    window.SetRenderer(this);

    pipelineCache = device.createPipelineCache(vk::PipelineCacheCreateInfo());

    vk::Semaphore presentSempaphore = device.createSemaphore(vk::SemaphoreCreateInfo());
    vk::Fence fence = device.createFence(vk::FenceCreateInfo());

    currentSwap = device.acquireNextImageKHR(swapChain, UINT64_MAX, presentSempaphore, fence).value; // Get swap image
}

VulkanRenderer::~VulkanRenderer()
{
    delete depthBuffer;

    // destroy swap chain and frame buffers
    device.destroySwapchainKHR(swapChain);
    for (auto& i : swapChainList)
    {
        device.destroyImageView(i->view);
    };

    for (unsigned int i = 0; i < numFrameBuffers; ++i)
    {
        device.destroyFramebuffer(frameBuffers[i]);
    }

    device.destroyDescriptorPool(defaultDescriptorPool);
    device.destroyCommandPool(commandPool);
    device.destroyPipeline(graphicsPipeline.pipeline);
    device.destroyRenderPass(defaultRenderPass);
    device.destroyPipelineCache(pipelineCache);
    device.destroy(); // Destroy everything except instance before this gets destroyed!

    instance.destroySurfaceKHR(surface);

    instance.destroyDebugUtilsMessengerEXT(debugMessenger, nullptr, *dispatcher);
    delete dispatcher;

    instance.destroy();

    delete[] frameBuffers;
}

bool VulkanRenderer::InitVulkan()
{
    InitInstance();
    InitDebugMessager();
    InitSurface();
    InitPhysicalDevice();
    InitLogicalDevice();

    // InitSwapChain();

    // Create the default render pass
    // InitDefaultRenderPass();

    // create graphics pipeline
    // CreateGraphicsPipeline();

    InitCommandPool();
    InitDefaultDescriptorPool();
    return true;
}

bool CheckLayerSupport(const std::vector<const char*>& layers)
{
    std::vector<vk::LayerProperties> availableLayers = vk::enumerateInstanceLayerProperties();

    for (const char* layerName : layers)
    {
        bool layerFound = false;

        for (const auto& layerProperties : availableLayers)
        {
            if (strcmp(layerName, layerProperties.layerName) == 0)
            {
                layerFound = true;
                break;
            }
        }

        if (!layerFound)
        {
            return false;
        }
    }

    return true;
}

std::vector<const char*> GetRequiredExtensions()
{
    // for window surface
    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
    };

#ifdef _WIN32
    extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif

    if (enableValidationLayers)
    {
        extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{
    std::cerr << "Nullus validation layer: " << pCallbackData->pMessage << std::endl;

    return VK_FALSE;
}

void PopulateDebugMessengerCreateInfo(vk::DebugUtilsMessengerCreateInfoEXT& createInfo)
{
    createInfo = {};
    createInfo.setMessageSeverity(vk::DebugUtilsMessageSeverityFlagBitsEXT::eError | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose)
        .setMessageType(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance)
        .setPfnUserCallback(debugCallback);
}

bool VulkanRenderer::InitInstance()
{
    std::vector<const char*> usedExtensions = GetRequiredExtensions();

    vk::ApplicationInfo appInfo = vk::ApplicationInfo()
                                      .setPApplicationName(this->hostWindow.GetTitle().c_str())
                                      .setApiVersion(VK_MAKE_VERSION(1, 1, 0));

    vk::InstanceCreateInfo instanceInfo = vk::InstanceCreateInfo()
                                              .setPApplicationInfo(&appInfo)
                                              .setEnabledExtensionCount(usedExtensions.size())
                                              .setPpEnabledExtensionNames(usedExtensions.data());

    vk::DebugUtilsMessengerCreateInfoEXT debugCreateInfo;
    if (enableValidationLayers)
    {
        if (CheckLayerSupport(validationLayers) == false)
        {
            std::cout << "Validation layers requested, but not available!" << std::endl;
            return false;
        }

        instanceInfo.setEnabledLayerCount(validationLayers.size());
        instanceInfo.setPpEnabledLayerNames(validationLayers.data());

        // Debugging instance creation and destruction
        PopulateDebugMessengerCreateInfo(debugCreateInfo);
        instanceInfo.pNext = &debugCreateInfo;
    }

    instance = vk::createInstance(instanceInfo);

    return true;
}

bool VulkanRenderer::InitDebugMessager()
{
    if (!enableValidationLayers)
    {
        return false;
    }

    vk::DebugUtilsMessengerCreateInfoEXT createInfo;
    PopulateDebugMessengerCreateInfo(createInfo);

    dispatcher = new vk::DispatchLoaderDynamic(instance, vkGetInstanceProcAddr, device);
    debugMessenger = instance.createDebugUtilsMessengerEXT(createInfo, nullptr, *dispatcher);

    return true;
}

bool VulkanRenderer::InitPhysicalDevice()
{
    auto enumResult = instance.enumeratePhysicalDevices();

    if (enumResult.empty())
    {
        return false; // Guess there's no Vulkan capable devices?!
    }

    gpu = enumResult[0];
    for (auto& i : enumResult)
    {
        vk::PhysicalDeviceProperties deviceProperties = i.getProperties();
        vk::PhysicalDeviceFeatures deviceFeatures = i.getFeatures();
        if (deviceProperties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu && deviceFeatures.geometryShader)
        {
            gpu = i; // Prefer a discrete GPU on multi device machines like laptops
        }
    }

    std::cout << "Vulkan using physical device " << gpu.getProperties().deviceName << std::endl;
    return true;
}

bool CheckDeviceExtensionSupport(vk::PhysicalDevice device)
{
    std::vector<vk::ExtensionProperties> availableExtensions = device.enumerateDeviceExtensionProperties();

    std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

    for (const auto& extension : availableExtensions)
    {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}

bool VulkanRenderer::InitLogicalDevice()
{
    InitDeviceQueueFamilies();

    CheckDeviceExtensionSupport(gpu);

    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {queueFamilyIndices.graphicsFamily.value(), queueFamilyIndices.presentFamily.value()};
    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies)
    {
        queueCreateInfos.push_back(vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = queueFamily,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority,
        });
    }

    vk::PhysicalDeviceFeatures features = vk::PhysicalDeviceFeatures()
                                              .setMultiDrawIndirect(true)
                                              .setDrawIndirectFirstInstance(true)
                                              .setShaderClipDistance(true)
                                              .setShaderCullDistance(true);

    vk::DeviceCreateInfo createInfo = vk::DeviceCreateInfo()
                                          .setQueueCreateInfoCount(queueCreateInfos.size())
                                          .setPQueueCreateInfos(queueCreateInfos.data())
                                          .setPEnabledFeatures(&features)
                                          .setEnabledExtensionCount(deviceExtensions.size())
                                          .setPpEnabledExtensionNames(deviceExtensions.data());

    if (enableValidationLayers)
    {
        createInfo.setEnabledLayerCount(validationLayers.size());
        createInfo.setPpEnabledLayerNames(validationLayers.data());
    }

    device = gpu.createDevice(createInfo);
    graphicsQueue = device.getQueue(queueFamilyIndices.graphicsFamily.value(), 0);
    presentQueue = device.getQueue(queueFamilyIndices.presentFamily.value(), 0);
    deviceMemoryProperties = gpu.getMemoryProperties();

    return true;
}

bool VulkanRenderer::InitDeviceQueueFamilies()
{
    vector<vk::QueueFamilyProperties> deviceQueueProps = gpu.getQueueFamilyProperties();

    for (unsigned int i = 0; i < deviceQueueProps.size(); ++i)
    {
        if (deviceQueueProps[i].queueFlags & vk::QueueFlagBits::eGraphics)
        {
            queueFamilyIndices.graphicsFamily = i;
            VkBool32 supportsPresent = gpu.getSurfaceSupportKHR(i, surface);
            if (supportsPresent)
            {
                queueFamilyIndices.presentFamily = i;
                break;
            }
        }
    }

    if (queueFamilyIndices.presentFamily.has_value() == false)
    {
        for (unsigned int i = 0; i < deviceQueueProps.size(); ++i)
        {
            VkBool32 supportsPresent = gpu.getSurfaceSupportKHR(i, surface);
            if (supportsPresent)
            {
                queueFamilyIndices.presentFamily = i;
                break;
            }
        }
    }

    return queueFamilyIndices.IsComplete();
}

bool VulkanRenderer::InitSurface()
{
#ifdef _WIN32
    Win32Window* window = (Win32Window*)&hostWindow;

    vk::Win32SurfaceCreateInfoKHR createInfo;

    createInfo = vk::Win32SurfaceCreateInfoKHR()
                     .setHinstance(window->GetInstance())
                     .setHwnd(window->GetHandle());

    surface = instance.createWin32SurfaceKHR(createInfo);
#endif

    if (!surface)
    {
        std::cout << "Failed to create surface!" << std::endl;
        return false;
    }

    return true;
}

int VulkanRenderer::InitSwapChain()
{
    vk::SwapchainKHR oldChain = swapChain;
    std::vector<SwapChain*> oldSwapChainList = swapChainList;
    swapChainList.clear();

    // choose swap surface format
    {
        auto formats = gpu.getSurfaceFormatsKHR(surface);
        if (formats.size() == 0)
        {
            std::cout << "No surface formats available!" << std::endl;
            return 0;
        }

        if (formats.size() == 1 && formats[0].format == vk::Format::eUndefined)
        {
            surfaceFormat = vk::Format::eB8G8R8A8Unorm;
            surfaceSpace = formats[0].colorSpace;
        }
        else
        {
            surfaceFormat = formats[0].format;
            surfaceSpace = formats[0].colorSpace;
        }
    }

    // choose present mode
    vk::PresentModeKHR idealPresentMode = vk::PresentModeKHR::eFifo;
    {
        auto presentModes = gpu.getSurfacePresentModesKHR(surface); // Type is of vector of PresentModeKHR

        for (const auto& i : presentModes)
        {
            if (i == vk::PresentModeKHR::eMailbox)
            {
                idealPresentMode = i;
                break;
            }
            else if (i == vk::PresentModeKHR::eImmediate)
            {
                idealPresentMode = vk::PresentModeKHR::eImmediate; // Might still become mailbox...
            }
        }
    }

    vk::SurfaceCapabilitiesKHR surfaceCaps = gpu.getSurfaceCapabilitiesKHR(surface);

    vk::SurfaceTransformFlagBitsKHR idealTransform;
    if (surfaceCaps.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity)
    {
        idealTransform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    }
    else
    {
        idealTransform = surfaceCaps.currentTransform;
    }

    int idealImageCount = surfaceCaps.minImageCount + 1;
    if (surfaceCaps.maxImageCount > 0 && idealImageCount > surfaceCaps.maxImageCount)
    {
        idealImageCount = Maths::Min(idealImageCount, (int)surfaceCaps.maxImageCount);
    }

    vk::Extent2D swapExtents = ChooseSwapExtent(surfaceCaps);

    vk::SwapchainCreateInfoKHR swapInfo;
    swapInfo.setPresentMode(idealPresentMode)
        .setPreTransform(idealTransform)
        .setSurface(surface)
        .setImageColorSpace(surfaceSpace)
        .setImageFormat(surfaceFormat)
        .setImageExtent(swapExtents)
        .setMinImageCount(idealImageCount)
        .setOldSwapchain(oldChain)
        .setImageArrayLayers(1)
        .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
        .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
        .setClipped(true);

    if (queueFamilyIndices.graphicsFamily != queueFamilyIndices.presentFamily)
    {
        uint32_t queueFamilyIndicesArray[] = {queueFamilyIndices.graphicsFamily.value(), queueFamilyIndices.presentFamily.value()};
        swapInfo.setImageSharingMode(vk::SharingMode::eConcurrent)
            .setQueueFamilyIndexCount(2)
            .setPQueueFamilyIndices(queueFamilyIndicesArray);
    }
    else
    {
        swapInfo.setImageSharingMode(vk::SharingMode::eExclusive)
            .setQueueFamilyIndexCount(0)
            .setPQueueFamilyIndices(nullptr);
    }

    swapChain = device.createSwapchainKHR(swapInfo);

    if (!oldSwapChainList.empty())
    {
        for (unsigned int i = 0; i < numFrameBuffers; ++i)
        {
            device.destroyImageView(oldSwapChainList[i]->view);
            delete oldSwapChainList[i];
        }
    }
    if (oldChain)
    {
        device.destroySwapchainKHR(oldChain);
    }

    auto images = device.getSwapchainImagesKHR(swapChain);

    for (auto& i : images)
    {
        vk::ImageViewCreateInfo viewCreate = vk::ImageViewCreateInfo()
                                                 .setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1))
                                                 .setFormat(surfaceFormat)
                                                 .setImage(i)
                                                 .setViewType(vk::ImageViewType::e2D);

        SwapChain* chain = new SwapChain();

        chain->image = i;

        ImageTransitionBarrier(&setupCmdBuffer, i, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageAspectFlagBits::eColor, vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eColorAttachmentOutput);

        chain->view = device.createImageView(viewCreate);

        swapChainList.push_back(chain);
    }

    return (int)images.size();
}

vk::Extent2D VulkanRenderer::ChooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities)
{
    if (capabilities.currentExtent.width != -1)
    {
        return capabilities.currentExtent;
    }
    else
    {
        int width = hostWindow.GetScreenSize().x, height = hostWindow.GetScreenSize().y;

        vk::Extent2D actualExtent = {
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height)};

        actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

        return actualExtent;
        return vk::Extent2D();
    }
}

void VulkanRenderer::ImageTransitionBarrier(vk::CommandBuffer* buffer, vk::Image image, vk::ImageLayout oldLayout, vk::ImageLayout newLayout, vk::ImageAspectFlags aspect, vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage, int mipLevel, int layer)
{
    if (!buffer)
    {
        buffer = &setupCmdBuffer;
    }
    vk::ImageSubresourceRange subRange = vk::ImageSubresourceRange(aspect, mipLevel, 1, layer, 1);

    vk::ImageMemoryBarrier memoryBarrier = vk::ImageMemoryBarrier()
                                               .setSubresourceRange(subRange)
                                               .setImage(image)
                                               .setOldLayout(oldLayout)
                                               .setNewLayout(newLayout);

    if (newLayout == vk::ImageLayout::eTransferDstOptimal)
    {
        memoryBarrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    }
    else if (newLayout == vk::ImageLayout::eTransferSrcOptimal)
    {
        memoryBarrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    }
    else if (newLayout == vk::ImageLayout::eColorAttachmentOptimal)
    {
        memoryBarrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    }
    else if (newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal)
    {
        memoryBarrier.dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    }
    else if (newLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
    {
        memoryBarrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eInputAttachmentRead; // added last bit?!?
    }

    buffer->pipelineBarrier(srcStage, dstStage, vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1, &memoryBarrier);
}

void VulkanRenderer::ImageTransitionBarrier(vk::CommandBuffer* buffer, VulkanTexture* t, vk::ImageLayout oldLayout, vk::ImageLayout newLayout, vk::ImageAspectFlags aspect, vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage, int mipLevel, int layer)
{
    if (!buffer)
    {
        buffer = &setupCmdBuffer;
    }
    ImageTransitionBarrier(buffer, t->GetImage(), oldLayout, newLayout, aspect, srcStage, dstStage, mipLevel, layer);
    t->layout = newLayout;
}

void VulkanRenderer::InitCommandPool()
{
    commandPool = device.createCommandPool(vk::CommandPoolCreateInfo{
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = queueFamilyIndices.graphicsFamily.value(),
    });

    auto buffers = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
        .commandPool = commandPool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    });

    frameCmdBuffer = buffers[0];
}

vk::CommandBuffer VulkanRenderer::BeginCmdBuffer()
{
    vk::CommandBufferAllocateInfo bufferInfo = {
        .commandPool = commandPool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };

    auto buffers = device.allocateCommandBuffers(bufferInfo); // Func returns a vector!

    vk::CommandBuffer& newBuf = buffers[0];

    vk::CommandBufferBeginInfo beginInfo = vk::CommandBufferBeginInfo();

    newBuf.begin(beginInfo);
    return newBuf;
}

void VulkanRenderer::EndCmdBufferWait(vk::CommandBuffer& buffer)
{
    vk::Fence fence = EndCmdBuffer(buffer);

    if (!fence)
    {
        return;
    }

    if (device.waitForFences(1, &fence, true, UINT64_MAX) != vk::Result::eSuccess)
    {
        std::cout << "Device queue submission taking too long?" << std::endl;
    };

    device.freeCommandBuffers(commandPool, buffer);

    device.destroyFence(fence);
}

vk::Fence VulkanRenderer::EndCmdBuffer(vk::CommandBuffer& buffer)
{
    vk::Fence fence;
    if (buffer)
    {
        buffer.end();
    }
    else
    {
        return fence;
    }

    fence = device.createFence(vk::FenceCreateInfo());

    vk::SubmitInfo submitInfo = vk::SubmitInfo();
    submitInfo.setCommandBufferCount(1);
    submitInfo.setPCommandBuffers(&buffer);

    graphicsQueue.submit(submitInfo, fence);
    return fence;
}

void VulkanRenderer::DestroyCmdBuffer(vk::CommandBuffer& buffer)
{
}

void VulkanRenderer::BeginSetupCmdBuffer()
{
    setupCmdBuffer = BeginCmdBuffer();
}

void VulkanRenderer::EndSetupCmdBuffer()
{
    EndCmdBufferWait(setupCmdBuffer);
}

void VulkanRenderer::OnWindowResize(int width, int height)
{
    if (width == currentWidth && height == currentHeight)
    {
        return;
    }
    currentWidth = width;
    currentHeight = height;

    defaultViewport = vk::Viewport(0.0f, (float)currentHeight, (float)currentWidth, -(float)currentHeight, 0.0f, 1.0f);
    defaultScissor = vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(currentWidth, currentHeight));

    defaultClearValues[0] = vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.2f, 0.2f, 0.2f, 1.0f}));
    defaultClearValues[1] = vk::ClearValue(vk::ClearDepthStencilValue(1.0f, 0));
    BeginSetupCmdBuffer();
    std::cout << "calling resize! new dimensions: " << currentWidth << " , " << currentHeight << std::endl;
    device.waitIdle();

    delete depthBuffer;
    depthBuffer = VulkanTexture::GenerateDepthTexture((int)hostWindow.GetScreenSize().x, (int)hostWindow.GetScreenSize().y);

    numFrameBuffers = InitSwapChain();

    InitDefaultRenderPass();
    CreateDefaultFrameBuffers();

    device.waitIdle();

    CompleteResize();

    EndSetupCmdBuffer();
}

ShaderBase* VulkanRenderer::CreateShader(const std::string& vertex, const std::string& fragment)
{
    VulkanShaderBuilder builder;
    builder.WithVertexBinary(vertex);
    builder.WithFragmentBinary(fragment);
    return builder.Build(*this);
}

void VulkanRenderer::DrawString(const std::string& text, const Maths::Vector2& pos, const Maths::Vector4& colour, float size)
{
    // todo:
}

void VulkanRenderer::DrawLine(const Maths::Vector3& start, const Maths::Vector3& end, const Maths::Vector4& colour)
{
    // todo:
}

void VulkanRenderer::CompleteResize()
{
}

void VulkanRenderer::BeginFrame()
{
    vk::CommandBufferInheritanceInfo inheritance;
    vk::CommandBufferBeginInfo bufferBegin = {.pInheritanceInfo = &inheritance};
    frameCmdBuffer.begin(bufferBegin);
    frameCmdBuffer.setViewport(0, 1, &defaultViewport);
    frameCmdBuffer.setScissor(0, 1, &defaultScissor);

    // Wait until the swap image is actually available!
    ImageTransitionBarrier(&frameCmdBuffer, swapChainList[currentSwap]->image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageAspectFlagBits::eColor, vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eColorAttachmentOutput);
    // frameCmdBuffer.beginRenderPass(defaultBeginInfo, vk::SubpassContents::eInline);
}

void VulkanRenderer::EndFrame()
{
    // frameCmdBuffer.endRenderPass();
    frameCmdBuffer.end();
    vk::SubmitInfo submitInfo = {
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &frameCmdBuffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    vk::Fence fence = device.createFence(vk::FenceCreateInfo());

    vk::Result result = graphicsQueue.submit(1, &submitInfo, fence);
    device.waitForFences(fence, true, ~0);
    device.destroyFence(fence);
}

void VulkanRenderer::SwapBuffers()
{
    PresentScreenImage();

    graphicsQueue.presentKHR(vk::PresentInfoKHR{
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .swapchainCount = 1,
        .pSwapchains = &swapChain,
        .pImageIndices = &currentSwap,
        .pResults = nullptr,
    });

    vk::Semaphore presentSempaphore = device.createSemaphore(vk::SemaphoreCreateInfo());
    vk::Fence fence = device.createFence(vk::FenceCreateInfo());

    currentSwap = device.acquireNextImageKHR(swapChain, UINT64_MAX, presentSempaphore, fence).value; // Get swap image

    defaultBeginInfo = vk::RenderPassBeginInfo()
                           .setRenderPass(defaultRenderPass)
                           .setFramebuffer(frameBuffers[currentSwap])
                           .setRenderArea(defaultScissor)
                           .setClearValueCount(sizeof(defaultClearValues) / sizeof(vk::ClearValue))
                           .setPClearValues(defaultClearValues);

    device.waitForFences(fence, true, ~0);

    device.destroySemaphore(presentSempaphore);
    device.destroyFence(fence);
}

void VulkanRenderer::CreateGraphicsPipeline()
{
    VulkanShader* shader = nullptr;
    VulkanMesh* mesh = nullptr;

    VulkanPipelineBuilder builder;
    builder.WithShaderState(shader);
    builder.WithVertexSpecification(mesh->GetVertexSpecification(), vk::PrimitiveTopology::eTriangleList);
    builder.WithRaster(vk::CullModeFlagBits::eBack, vk::PolygonMode::eFill);
    //builder.WithBlendState(vk::BlendFactor::eSrcAlpha, vk::BlendFactor::eOneMinusSrcAlpha, true);
    builder.WithDepthState(vk::CompareOp::eLess, true, true);
    builder.WithPass(defaultRenderPass);
    builder.WithDebugName("Default Pipeline");
    graphicsPipeline = builder.Build(*this);
}

void VulkanRenderer::InitDefaultRenderPass()
{
    if (defaultRenderPass)
    {
        device.destroyRenderPass(defaultRenderPass);
    }
    vk::AttachmentDescription attachments[] = {
        vk::AttachmentDescription()
            .setInitialLayout(vk::ImageLayout::eColorAttachmentOptimal)
            .setFinalLayout(vk::ImageLayout::eColorAttachmentOptimal)
            .setFormat(surfaceFormat)
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
            .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare),
        vk::AttachmentDescription()
            .setInitialLayout(vk::ImageLayout::eUndefined)
            .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
            .setFormat(depthBuffer->GetFormat())
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
            .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)};

    vk::AttachmentReference references[] = {
        vk::AttachmentReference(0, vk::ImageLayout::eColorAttachmentOptimal),
        vk::AttachmentReference(1, vk::ImageLayout::eDepthStencilAttachmentOptimal)};

    vk::SubpassDescription subPass = vk::SubpassDescription()
                                         .setColorAttachmentCount(1)
                                         .setPColorAttachments(&references[0])
                                         .setPDepthStencilAttachment(&references[1])
                                         .setPipelineBindPoint(vk::PipelineBindPoint::eGraphics);

    vk::RenderPassCreateInfo renderPassInfo = vk::RenderPassCreateInfo()
                                                  .setAttachmentCount(2)
                                                  .setPAttachments(attachments)
                                                  .setSubpassCount(1)
                                                  .setPSubpasses(&subPass);

    defaultRenderPass = device.createRenderPass(renderPassInfo);
}

void VulkanRenderer::PresentScreenImage()
{
    BeginSetupCmdBuffer();

    vk::ImageMemoryBarrier barrier = vk::ImageMemoryBarrier()
                                         .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
                                         .setDstAccessMask(vk::AccessFlagBits::eMemoryRead)
                                         .setImage(swapChainList[currentSwap]->image)
                                         .setOldLayout(vk::ImageLayout::eColorAttachmentOptimal)
                                         .setNewLayout(vk::ImageLayout::ePresentSrcKHR);

    barrier.setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

    setupCmdBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eBottomOfPipe, vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1, &barrier);

    EndSetupCmdBuffer();
}

bool VulkanRenderer::CreateDefaultFrameBuffers()
{
    if (frameBuffers)
    {
        for (unsigned int i = 0; i < numFrameBuffers; ++i)
        {
            device.destroyFramebuffer(frameBuffers[i]);
        }
    }
    else
    {
        frameBuffers = new vk::Framebuffer[numFrameBuffers];
    }

    vk::ImageView attachments[2];

    vk::FramebufferCreateInfo createInfo = vk::FramebufferCreateInfo()
                                               .setWidth((int)hostWindow.GetScreenSize().x)
                                               .setHeight((int)hostWindow.GetScreenSize().y)
                                               .setLayers(1)
                                               .setAttachmentCount(2)
                                               .setPAttachments(attachments)
                                               .setRenderPass(defaultRenderPass);

    for (unsigned int i = 0; i < numFrameBuffers; ++i)
    {
        attachments[0] = swapChainList[i]->view;
        attachments[1] = depthBuffer->defaultView;
        frameBuffers[i] = device.createFramebuffer(createInfo);
    }

    defaultBeginInfo = vk::RenderPassBeginInfo()
                           .setRenderPass(defaultRenderPass)
                           .setFramebuffer(frameBuffers[currentSwap])
                           .setRenderArea(defaultScissor)
                           .setClearValueCount(sizeof(defaultClearValues) / sizeof(vk::ClearValue))
                           .setPClearValues(defaultClearValues);

    return true;
}

bool VulkanRenderer::MemoryTypeFromPhysicalDeviceProps(vk::MemoryPropertyFlags requirements, uint32_t type, uint32_t& index)
{
    for (int i = 0; i < 32; ++i)
    {
        if ((type & 1) == 1)
        { // We care about this requirement
            if ((deviceMemoryProperties.memoryTypes[i].propertyFlags & requirements) == requirements)
            {
                index = i;
                return true;
            }
        }
        type >>= 1; // Check next bit
    }
    return false;
}

void VulkanRenderer::InitDefaultDescriptorPool()
{
    int maxSets = 128; // how many times can we ask the pool for a descriptor set?
    vk::DescriptorPoolSize poolSizes[] = {
        vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 128),
        vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 128)};

    vk::DescriptorPoolCreateInfo poolCreate;
    poolCreate.setPoolSizeCount(sizeof(poolSizes) / sizeof(vk::DescriptorPoolSize));
    poolCreate.setPPoolSizes(poolSizes);
    poolCreate.setMaxSets(maxSets);

    defaultDescriptorPool = device.createDescriptorPool(poolCreate);
}

void VulkanRenderer::SetDebugName(vk::ObjectType t, uint64_t handle, const string& debugName)
{
    device.setDebugUtilsObjectNameEXT(
        vk::DebugUtilsObjectNameInfoEXT()
            .setObjectType(t)
            .setObjectHandle(handle)
            .setPObjectName(debugName.c_str()),
        *dispatcher);
};

void VulkanRenderer::UpdateImageDescriptor(vk::DescriptorSet& set, VulkanTexture* tex, vk::Sampler sampler, vk::ImageView forceView, vk::ImageLayout forceLayout, int bindingNum)
{
    vk::DescriptorImageInfo imageInfo = vk::DescriptorImageInfo()
                                            .setSampler(sampler)
                                            .setImageView(forceView)
                                            .setImageLayout(forceLayout);

    vk::WriteDescriptorSet descriptorWrite = vk::WriteDescriptorSet()
                                                 .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                                                 .setDstSet(set)
                                                 .setDstBinding(bindingNum)
                                                 .setDescriptorCount(1)
                                                 .setPImageInfo(&imageInfo);

    device.updateDescriptorSets(1, &descriptorWrite, 0, nullptr);
}

void VulkanRenderer::UpdateImageDescriptor(vk::DescriptorSet& set, VulkanTexture* tex, vk::Sampler sampler, int bindingNum)
{
    if (!tex)
    {
        std::cout << __FUNCTION__ << " tex parameter is NULL!\n";
        return;
    }

    vk::DescriptorImageInfo imageInfo = vk::DescriptorImageInfo()
                                            .setSampler(sampler)
                                            .setImageView(tex->GetDefaultView())
                                            .setImageLayout(tex->GetLayout());

    vk::WriteDescriptorSet descriptorWrite = vk::WriteDescriptorSet()
                                                 .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                                                 .setDstSet(set)
                                                 .setDstBinding(bindingNum)
                                                 .setDescriptorCount(1)
                                                 .setPImageInfo(&imageInfo);

    device.updateDescriptorSets(1, &descriptorWrite, 0, nullptr);
}

void VulkanRenderer::InitUniformBuffer(UniformData& uniform, void* data, int dataSize)
{
    uniform.buffer = device.createBuffer(vk::BufferCreateInfo{
        .size = (uint32_t)dataSize,
        .usage = vk::BufferUsageFlagBits::eUniformBuffer,
    });

    uniform.descriptorInfo.buffer = uniform.buffer;
    uniform.descriptorInfo.range = dataSize; // reqs.size;

    vk::MemoryRequirements reqs = device.getBufferMemoryRequirements(uniform.buffer);
    uniform.allocInfo = {.allocationSize = reqs.size};

    bool found = MemoryTypeFromPhysicalDeviceProps(vk::MemoryPropertyFlagBits::eHostVisible, reqs.memoryTypeBits, uniform.allocInfo.memoryTypeIndex);

    uniform.deviceMem = device.allocateMemory(uniform.allocInfo);

    device.bindBufferMemory(uniform.buffer, uniform.deviceMem, 0);

    UpdateUniformBuffer(uniform, data, dataSize);
}

void VulkanRenderer::UpdateUniformBuffer(UniformData& uniform, void* data, int dataSize)
{
    void* mappedData = device.mapMemory(uniform.deviceMem, 0, uniform.allocInfo.allocationSize);
    memcpy(mappedData, data, dataSize);
    device.unmapMemory(uniform.deviceMem);
}

vk::DescriptorSet VulkanRenderer::BuildDescriptorSet(vk::DescriptorSetLayout& layout)
{
    vk::DescriptorSetAllocateInfo allocateInfo = vk::DescriptorSetAllocateInfo()
                                                     .setDescriptorPool(defaultDescriptorPool)
                                                     .setDescriptorSetCount(1)
                                                     .setPSetLayouts(&layout);

    vk::DescriptorSet newSet;
    device.allocateDescriptorSets(&allocateInfo, &newSet);
    return newSet;
}

void VulkanRenderer::SubmitDrawCall(VulkanMesh* m, vk::CommandBuffer& to)
{
    VkDeviceSize baseOffset = 0;
    int instanceCount = 1;

    to.bindVertexBuffers(0, 1, &m->GetVertexBuffer(), &baseOffset);

    if (m->GetIndexCount() > 0)
    {
        to.bindIndexBuffer(m->GetIndexBuffer(), 0, vk::IndexType::eUint32);

        to.drawIndexed(m->GetIndexCount(), instanceCount, 0, 0, 0);
    }
    else
    {
        to.draw(m->GetVertexCount(), instanceCount, 0, 0);
    }
}
