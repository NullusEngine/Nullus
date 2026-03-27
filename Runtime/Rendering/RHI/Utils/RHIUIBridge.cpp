#include "Rendering/RHI/Utils/RHIUIBridge.h"

#include <unordered_map>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Debug/Logger.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "ImGui/backends/imgui_impl_glfw.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "ImGui/backends/imgui_impl_dx12.h"
#endif
#if NLS_HAS_VULKAN && NLS_HAS_IMGUI_VULKAN_BACKEND
#include <vulkan/vulkan.h>
#include "ImGui/backends/imgui_impl_vulkan.h"
#endif

namespace NLS::Render::RHI
{
    namespace
    {
        class NullUIBridge final : public RHIUIBridge
        {
        public:
            bool HasRendererBackend() const override { return false; }
            void BeginFrame() override {}
            void RenderDrawData(ImDrawData*) override {}
            void* ResolveTextureID(uint32_t) override { return nullptr; }
        };

        class OpenGLUIBridge final : public RHIUIBridge
        {
        public:
            explicit OpenGLUIBridge(const std::string& glslVersion)
                : m_initialized(ImGui_ImplOpenGL3_Init(glslVersion.c_str()))
            {
            }

            ~OpenGLUIBridge() override
            {
                if (m_initialized)
                    ImGui_ImplOpenGL3_Shutdown();
            }

            bool HasRendererBackend() const override { return m_initialized; }

            void BeginFrame() override
            {
                if (m_initialized)
                    ImGui_ImplOpenGL3_NewFrame();
            }

            void RenderDrawData(ImDrawData* drawData) override
            {
                if (m_initialized && drawData != nullptr)
                    ImGui_ImplOpenGL3_RenderDrawData(drawData);
            }

            void* ResolveTextureID(uint32_t textureId) override
            {
                if (NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>())
                    return NLS_SERVICE(NLS::Render::Context::Driver).GetUITextureHandle(textureId);

                return textureId != 0 && textureId != static_cast<uint32_t>(-1)
                    ? reinterpret_cast<void*>(static_cast<intptr_t>(textureId))
                    : nullptr;
            }

        private:
            bool m_initialized = false;
        };

#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND
        class DX12UIBridge final : public RHIUIBridge
        {
        public:
            explicit DX12UIBridge(const NativeRenderDeviceInfo& nativeInfo)
            {
                Initialize(nativeInfo);
            }

            ~DX12UIBridge() override
            {
                Shutdown();
            }

            bool HasRendererBackend() const override { return m_initialized; }

            void BeginFrame() override
            {
                if (m_initialized)
                    ImGui_ImplDX12_NewFrame();
            }

            void NotifySwapchainWillResize() override
            {
                ReleaseSwapchainRenderResources();
            }

            void RenderDrawData(ImDrawData* drawData) override
            {
                if (!m_initialized || drawData == nullptr)
                    return;

                if (!NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>())
                    return;

                auto& driver = NLS_SERVICE(NLS::Render::Context::Driver);
                if (!EnsureSwapchainRenderResources(driver.GetNativeDeviceInfo()))
                    return;

                if (!driver.PrepareUIRender())
                    return;

                const UINT backBufferIndex = m_swapchain->GetCurrentBackBufferIndex();
                auto& commandAllocator = m_commandAllocators[backBufferIndex];
                auto& backBuffer = m_backBuffers[backBufferIndex];

                commandAllocator->Reset();
                m_commandList->Reset(commandAllocator.Get(), nullptr);

                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = backBuffer.Get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                m_commandList->ResourceBarrier(1, &barrier);

                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                rtvHandle.ptr += static_cast<SIZE_T>(backBufferIndex) * m_rtvDescriptorSize;
                m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
                m_commandList->SetDescriptorHeaps(1, heaps);
                ImGui_ImplDX12_RenderDrawData(drawData, m_commandList.Get());

                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                m_commandList->ResourceBarrier(1, &barrier);
                m_commandList->Close();

                ID3D12CommandList* commandLists[] = { m_commandList.Get() };
                m_queue->ExecuteCommandLists(1, commandLists);

                const UINT64 fenceValue = ++m_fenceValue;
                m_queue->Signal(m_fence.Get(), fenceValue);
                if (m_fence->GetCompletedValue() < fenceValue)
                {
                    m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
                    WaitForSingleObject(m_fenceEvent, INFINITE);
                }
            }

            void* ResolveTextureID(uint32_t textureId) override
            {
                if (!m_initialized || textureId == 0)
                    return nullptr;

                if (!NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>())
                    return nullptr;

                auto& driver = NLS_SERVICE(NLS::Render::Context::Driver);
                auto* nativeTexture = static_cast<ID3D12Resource*>(driver.GetUITextureHandle(textureId));
                if (!nativeTexture)
                    return nullptr;

                const auto resourceDesc = nativeTexture->GetDesc();
                if (resourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                    return nullptr;

                auto found = m_textureHandles.find(textureId);
                if (found == m_textureHandles.end())
                {
                    if (m_nextTextureDescriptorIndex >= m_srvDescriptorCapacity)
                        return nullptr;

                    CachedTextureHandle cachedHandle{};
                    cachedHandle.resource = nativeTexture;
                    cachedHandle.descriptorIndex = m_nextTextureDescriptorIndex++;
                    cachedHandle.gpuHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
                    cachedHandle.gpuHandle.ptr += static_cast<UINT64>(cachedHandle.descriptorIndex) * m_srvDescriptorSize;
                    found = m_textureHandles.emplace(textureId, cachedHandle).first;
                }

                auto& cachedHandle = found->second;
                if (cachedHandle.resource != nativeTexture)
                    cachedHandle.resource = nativeTexture;

                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Format = resourceDesc.Format;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = resourceDesc.MipLevels;

                auto cpuHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                cpuHandle.ptr += static_cast<SIZE_T>(cachedHandle.descriptorIndex) * m_srvDescriptorSize;
                m_device->CreateShaderResourceView(nativeTexture, &srvDesc, cpuHandle);
                return reinterpret_cast<void*>(cachedHandle.gpuHandle.ptr);
            }

        private:
            bool EnsureSwapchainRenderResources(const NativeRenderDeviceInfo& nativeInfo)
            {
                if (!m_device || !m_queue)
                    return false;

                auto* swapchain = static_cast<IDXGISwapChain3*>(nativeInfo.swapchain);
                if (swapchain == nullptr)
                    return false;

                m_swapchain = swapchain;

                const uint32_t targetImageCount = nativeInfo.swapchainImageCount > 0 ? nativeInfo.swapchainImageCount : 2u;
                if (m_commandList != nullptr &&
                    !m_backBuffers.empty() &&
                    m_backBuffers.size() == targetImageCount &&
                    m_commandAllocators.size() == targetImageCount)
                {
                    return true;
                }

                ReleaseSwapchainRenderResources();

                D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
                rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                rtvHeapDesc.NumDescriptors = targetImageCount;
                rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                if (FAILED(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap))))
                {
                    NLS_LOG_WARNING("DX12UIBridge failed to create RTV heap.");
                    return false;
                }

                m_imageCount = targetImageCount;
                m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                m_backBuffers.resize(m_imageCount);
                m_commandAllocators.resize(m_imageCount);

                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                for (uint32_t i = 0; i < m_imageCount; ++i)
                {
                    if (FAILED(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]))))
                    {
                        NLS_LOG_WARNING("DX12UIBridge failed to fetch swapchain backbuffer " + std::to_string(i) + ".");
                        ReleaseSwapchainRenderResources();
                        return false;
                    }

                    m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvHandle);
                    rtvHandle.ptr += m_rtvDescriptorSize;

                    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i]))))
                    {
                        NLS_LOG_WARNING("DX12UIBridge failed to create command allocator " + std::to_string(i) + ".");
                        ReleaseSwapchainRenderResources();
                        return false;
                    }
                }

                if (FAILED(m_device->CreateCommandList(
                    0,
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    m_commandAllocators[0].Get(),
                    nullptr,
                    IID_PPV_ARGS(&m_commandList))))
                {
                    NLS_LOG_WARNING("DX12UIBridge failed to create graphics command list.");
                    ReleaseSwapchainRenderResources();
                    return false;
                }
                m_commandList->Close();
                return true;
            }

            void ReleaseSwapchainRenderResources()
            {
                if (m_fence && m_queue && m_fenceEvent != nullptr)
                {
                    const UINT64 fenceValue = ++m_fenceValue;
                    m_queue->Signal(m_fence.Get(), fenceValue);
                    if (m_fence->GetCompletedValue() < fenceValue)
                    {
                        m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
                        WaitForSingleObject(m_fenceEvent, INFINITE);
                    }
                }

                m_commandAllocators.clear();
                m_backBuffers.clear();
                m_commandList.Reset();
                m_rtvHeap.Reset();
            }

            bool Initialize(const NativeRenderDeviceInfo& nativeInfo)
            {
                m_device = static_cast<ID3D12Device*>(nativeInfo.device);
                m_queue = static_cast<ID3D12CommandQueue*>(nativeInfo.graphicsQueue);
                m_swapchain = static_cast<IDXGISwapChain3*>(nativeInfo.swapchain);
                m_imageCount = nativeInfo.swapchainImageCount > 0 ? nativeInfo.swapchainImageCount : 2u;

                if (!m_device || !m_queue || !m_swapchain)
                {
                    NLS_LOG_WARNING("DX12UIBridge initialization skipped because device, queue, or swapchain is missing.");
                    return false;
                }

                D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
                srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                srvHeapDesc.NumDescriptors = 256;
                srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (FAILED(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap))))
                {
                    NLS_LOG_WARNING("DX12UIBridge failed to create shader-visible SRV heap.");
                    return false;
                }
                m_srvDescriptorCapacity = srvHeapDesc.NumDescriptors;
                m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
                {
                    NLS_LOG_WARNING("DX12UIBridge failed to create fence.");
                    return false;
                }

                m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (m_fenceEvent == nullptr)
                {
                    NLS_LOG_WARNING("DX12UIBridge failed to create fence event.");
                    return false;
                }

                if (!ImGui_ImplDX12_Init(
                    m_device.Get(),
                    static_cast<int>(m_imageCount),
                    m_backbufferFormat,
                    m_srvHeap.Get(),
                    m_srvHeap->GetCPUDescriptorHandleForHeapStart(),
                    m_srvHeap->GetGPUDescriptorHandleForHeapStart()))
                {
                    NLS_LOG_WARNING("DX12UIBridge failed inside ImGui_ImplDX12_Init().");
                    return false;
                }

                if (!EnsureSwapchainRenderResources(nativeInfo))
                    return false;

                m_initialized = true;
                return true;
            }

            void Shutdown()
            {
                if (m_initialized)
                    ImGui_ImplDX12_Shutdown();

                if (m_fence && m_queue)
                {
                    const UINT64 fenceValue = ++m_fenceValue;
                    m_queue->Signal(m_fence.Get(), fenceValue);
                    if (m_fence->GetCompletedValue() < fenceValue)
                    {
                        m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
                        WaitForSingleObject(m_fenceEvent, INFINITE);
                    }
                }

                if (m_fenceEvent != nullptr)
                {
                    CloseHandle(m_fenceEvent);
                    m_fenceEvent = nullptr;
                }

                ReleaseSwapchainRenderResources();
                m_textureHandles.clear();
                m_srvHeap.Reset();
                m_swapchain.Reset();
                m_queue.Reset();
                m_device.Reset();
                m_fence.Reset();
                m_initialized = false;
            }

        private:
            Microsoft::WRL::ComPtr<ID3D12Device> m_device;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
            Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
            Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
            std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> m_commandAllocators;
            std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_backBuffers;
            struct CachedTextureHandle
            {
                ID3D12Resource* resource = nullptr;
                D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
                UINT descriptorIndex = 0;
            };

            std::unordered_map<uint32_t, CachedTextureHandle> m_textureHandles;
            HANDLE m_fenceEvent = nullptr;
            UINT64 m_fenceValue = 0;
            UINT m_rtvDescriptorSize = 0;
            UINT m_srvDescriptorSize = 0;
            UINT m_srvDescriptorCapacity = 0;
            UINT m_nextTextureDescriptorIndex = 1;
            DXGI_FORMAT m_backbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            uint32_t m_imageCount = 0;
            bool m_initialized = false;
        };
#endif

#if NLS_HAS_VULKAN && NLS_HAS_IMGUI_VULKAN_BACKEND
        class VulkanUIBridge final : public RHIUIBridge
        {
        public:
            explicit VulkanUIBridge(const NativeRenderDeviceInfo& nativeInfo)
            {
                Initialize(nativeInfo);
            }

            ~VulkanUIBridge() override
            {
                if (m_initialized)
                {
                    if (NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>())
                        NLS_SERVICE(NLS::Render::Context::Driver).ReleaseUITextureHandles();
                    ImGui_ImplVulkan_Shutdown();
                }
            }

            bool HasRendererBackend() const override { return m_initialized; }

            void BeginFrame() override
            {
                if (!m_initialized)
                    return;

                if (NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>())
                {
                    const auto nativeInfo = NLS_SERVICE(NLS::Render::Context::Driver).GetNativeDeviceInfo();
                    const uint32_t imageCount = nativeInfo.swapchainImageCount > 0 ? nativeInfo.swapchainImageCount : 2u;
                    if (m_imageCount != imageCount)
                    {
                        ImGui_ImplVulkan_SetMinImageCount(imageCount);
                        m_imageCount = imageCount;
                    }
                }

                ImGui_ImplVulkan_NewFrame();
            }

            void RenderDrawData(ImDrawData* drawData) override
            {
                if (!m_initialized || drawData == nullptr)
                    return;

                if (!NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>())
                    return;

                auto& driver = NLS_SERVICE(NLS::Render::Context::Driver);
                if (!driver.PrepareUIRender())
                    return;

                const auto nativeInfo = driver.GetNativeDeviceInfo();
                const auto commandBuffer = reinterpret_cast<VkCommandBuffer>(nativeInfo.currentCommandBuffer);
                if (commandBuffer != VK_NULL_HANDLE)
                    ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
            }

            void* ResolveTextureID(uint32_t textureId) override
            {
                if (NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>())
                    return NLS_SERVICE(NLS::Render::Context::Driver).GetUITextureHandle(textureId);
                return nullptr;
            }

        private:
            static void CheckVulkanResult(const VkResult result)
            {
                if (result != VK_SUCCESS)
                    NLS_LOG_ERROR("ImGui Vulkan backend reported VkResult=" + std::to_string(static_cast<int>(result)));
            }

            bool Initialize(const NativeRenderDeviceInfo& nativeInfo)
            {
                const auto instance = reinterpret_cast<VkInstance>(nativeInfo.instance);
                const auto physicalDevice = reinterpret_cast<VkPhysicalDevice>(nativeInfo.physicalDevice);
                const auto device = reinterpret_cast<VkDevice>(nativeInfo.device);
                const auto queue = reinterpret_cast<VkQueue>(nativeInfo.graphicsQueue);
                const auto renderPass = reinterpret_cast<VkRenderPass>(nativeInfo.uiRenderPass);
                const auto descriptorPool = reinterpret_cast<VkDescriptorPool>(nativeInfo.uiDescriptorPool);
                const uint32_t imageCount = nativeInfo.swapchainImageCount > 0 ? nativeInfo.swapchainImageCount : 2u;

                if (instance == VK_NULL_HANDLE ||
                    physicalDevice == VK_NULL_HANDLE ||
                    device == VK_NULL_HANDLE ||
                    queue == VK_NULL_HANDLE ||
                    renderPass == VK_NULL_HANDLE ||
                    descriptorPool == VK_NULL_HANDLE)
                {
                    return false;
                }

                ImGui_ImplVulkan_InitInfo initInfo{};
                initInfo.Instance = instance;
                initInfo.PhysicalDevice = physicalDevice;
                initInfo.Device = device;
                initInfo.QueueFamily = nativeInfo.graphicsQueueFamilyIndex;
                initInfo.Queue = queue;
                initInfo.DescriptorPool = descriptorPool;
                initInfo.RenderPass = renderPass;
                initInfo.MinImageCount = imageCount;
                initInfo.ImageCount = imageCount;
                initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
                initInfo.MinAllocationSize = 1024 * 1024;
                initInfo.CheckVkResultFn = CheckVulkanResult;

                if (!ImGui_ImplVulkan_Init(&initInfo))
                    return false;

                m_imageCount = imageCount;
                m_initialized = true;
                return true;
            }

        private:
            uint32_t m_imageCount = 0;
            bool m_initialized = false;
        };
#endif
    }

    std::unique_ptr<RHIUIBridge> CreateRHIUIBridge(
        GLFWwindow*,
        NLS::Render::Settings::EGraphicsBackend backend,
        const NativeRenderDeviceInfo& nativeDeviceInfo,
        const std::string& glslVersion)
    {
        switch (backend)
        {
        case NLS::Render::Settings::EGraphicsBackend::OPENGL:
            return std::make_unique<OpenGLUIBridge>(glslVersion);
#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND
        case NLS::Render::Settings::EGraphicsBackend::DX12:
            return std::make_unique<DX12UIBridge>(nativeDeviceInfo);
#endif
#if NLS_HAS_VULKAN && NLS_HAS_IMGUI_VULKAN_BACKEND
        case NLS::Render::Settings::EGraphicsBackend::VULKAN:
            return std::make_unique<VulkanUIBridge>(nativeDeviceInfo);
#endif
        case NLS::Render::Settings::EGraphicsBackend::DX11:
        case NLS::Render::Settings::EGraphicsBackend::METAL:
        case NLS::Render::Settings::EGraphicsBackend::NONE:
        default:
            return std::make_unique<NullUIBridge>();
        }
    }
}
