#include "Rendering/RHI/Utils/RHIUIBridge.h"

#include <unordered_map>
#include <vector>

#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "ImGui/backends/imgui_impl_glfw.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"
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
        NLS::Render::Context::Driver* ResolveUIDriver()
        {
            return NLS::Render::Context::TryGetLocatedDriver();
        }

        NativeRenderDeviceInfo ResolveNativeDeviceInfo(const NLS::Render::Settings::EGraphicsBackend backend)
        {
            if (const auto* driver = ResolveUIDriver(); driver != nullptr)
            {
                // Only use driver's native info if it has explicit device
                if (NLS::Render::Context::DriverRendererAccess::HasExplicitRHI(*driver))
                    return NLS::Render::Context::DriverUIAccess::GetNativeDeviceInfo(*driver);
            }

            NativeRenderDeviceInfo nativeInfo;
            switch (backend)
            {
            case NLS::Render::Settings::EGraphicsBackend::DX11:
                nativeInfo.backend = NativeBackendType::DX11;
                break;
            case NLS::Render::Settings::EGraphicsBackend::DX12:
                nativeInfo.backend = NativeBackendType::DX12;
                break;
            case NLS::Render::Settings::EGraphicsBackend::VULKAN:
                nativeInfo.backend = NativeBackendType::Vulkan;
                break;
            case NLS::Render::Settings::EGraphicsBackend::OPENGL:
                nativeInfo.backend = NativeBackendType::OpenGL;
                break;
            case NLS::Render::Settings::EGraphicsBackend::METAL:
                nativeInfo.backend = NativeBackendType::Metal;
                break;
            case NLS::Render::Settings::EGraphicsBackend::NONE:
            default:
                nativeInfo.backend = NativeBackendType::None;
                break;
            }
            return nativeInfo;
        }

        class NullUIBridge final : public RHIUIBridge
        {
        public:
            bool HasRendererBackend() const override { return false; }
            void BeginFrame() override {}
            void RenderDrawData(ImDrawData*, uint32_t) override {}
            NativeHandle ResolveTextureView(const std::shared_ptr<RHITextureView>&) override { return {}; }
            void SetWaitSemaphore(void*) override {}
            void SetSignalSemaphore(void*) override {}
            void* GetUISignalSemaphore() override { return nullptr; }
            void SubmitCommandBuffer(uint32_t) override {}
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

            void RenderDrawData(ImDrawData* drawData, uint32_t) override
            {
                if (m_initialized && drawData != nullptr)
                    ImGui_ImplOpenGL3_RenderDrawData(drawData);
            }

            NativeHandle ResolveTextureView(const std::shared_ptr<RHITextureView>& textureView) override
            {
                if (const auto* driver = ResolveUIDriver(); driver != nullptr)
                {
                    void* handle = NLS::Render::Context::DriverUIAccess::GetUITextureHandle(*driver, textureView);
                    if (handle != nullptr)
                        return {NLS::Render::RHI::BackendType::OpenGL, handle};
                }

                // Fallback: use RHITextureView's native shader resource view
                if (textureView != nullptr)
                {
                    NLS::Render::RHI::NativeHandle nativeHandle = textureView->GetNativeShaderResourceView();
                    if (nativeHandle.IsValid())
                        return nativeHandle;
                }
                return {};
            }

            void SetWaitSemaphore(void*) override {}
            void SetSignalSemaphore(void*) override {}
            void* GetUISignalSemaphore() override { return nullptr; }
            void SubmitCommandBuffer(uint32_t) override {}

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
                if (!m_initialized)
                    return;

                ImGuiIO& io = ImGui::GetIO();

                // Ensure font atlas is built before NewFrame - fonts may have been loaded after Init
                if (!io.Fonts->IsBuilt())
                {
                    if (!io.Fonts->Build())
                    {
                        NLS_LOG_ERROR("DX12UIBridge::BeginFrame: io.Fonts->Build() failed");
                        m_initialized = false;
                        return;
                    }
                }

                ImGui_ImplDX12_NewFrame();
            }

            void NotifySwapchainWillResize() override
            {
                ReleaseSwapchainRenderResources();
            }

            void RenderDrawData(ImDrawData* drawData, uint32_t) override
            {
                if (!m_initialized || drawData == nullptr)
                    return;

                auto* driver = ResolveUIDriver();
                if (driver == nullptr)
                    return;

                auto nativeInfo = NLS::Render::Context::DriverUIAccess::GetNativeDeviceInfo(*driver);

                if (!EnsureSwapchainRenderResources(nativeInfo))
                    return;

                if (!NLS::Render::Context::DriverUIAccess::PrepareUIRender(*driver))
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

            NativeHandle ResolveTextureView(const std::shared_ptr<RHITextureView>& textureView) override
            {
                if (!m_initialized || textureView == nullptr)
                    return {};

                const auto texture = textureView->GetTexture();
                if (texture == nullptr)
                    return {};

                const NLS::Render::RHI::NativeHandle nativeImageHandle = texture->GetNativeImageHandle();
                if (!nativeImageHandle.IsValid() ||
                    nativeImageHandle.backend != NLS::Render::RHI::BackendType::DX12)
                {
                    return {};
                }

                auto* resource = static_cast<ID3D12Resource*>(nativeImageHandle.handle);
                if (resource == nullptr)
                    return {};

                const uintptr_t viewKey = reinterpret_cast<uintptr_t>(textureView.get());
                auto found = m_textureHandles.find(viewKey);
                if (found == m_textureHandles.end())
                {
                    if (m_nextTextureDescriptorIndex >= m_srvDescriptorCapacity)
                    {
                        NLS_LOG_WARNING("DX12UIBridge::ResolveTextureView: SRV heap capacity exhausted.");
                        return {};
                    }

                    const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(
                        texture->GetDesc(),
                        textureView->GetDesc());
                    if (!descriptors.hasSrv)
                        return {};

                    D3D12_CPU_DESCRIPTOR_HANDLE destinationCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                    destinationCpu.ptr += static_cast<SIZE_T>(m_nextTextureDescriptorIndex) * m_srvDescriptorSize;

                    D3D12_GPU_DESCRIPTOR_HANDLE destinationGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
                    destinationGpu.ptr += static_cast<UINT64>(m_nextTextureDescriptorIndex) * m_srvDescriptorSize;

                    m_device->CreateShaderResourceView(resource, &descriptors.srvDesc, destinationCpu);

                    CachedTextureHandle cachedHandle;
                    cachedHandle.resource = resource;
                    cachedHandle.gpuHandle = destinationGpu;
                    cachedHandle.descriptorIndex = m_nextTextureDescriptorIndex;
                    ++m_nextTextureDescriptorIndex;

                    found = m_textureHandles.emplace(viewKey, cachedHandle).first;
                }

                return {
                    NLS::Render::RHI::BackendType::DX12,
                    reinterpret_cast<void*>(found->second.gpuHandle.ptr)
                };
            }

            void SetWaitSemaphore(void* semaphore) override
            {
                m_waitSemaphore = semaphore;
            }

            void SetSignalSemaphore(void* semaphore) override
            {
                m_signalSemaphore = semaphore;
            }

            void* GetUISignalSemaphore() override
            {
                if (!m_initialized)
                    return nullptr;

                // Create DX12 fence for UI signal if not already created
                if (m_uiFence == nullptr)
                {
                    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_uiFence));
                    if (FAILED(hr))
                    {
                        NLS_LOG_ERROR("DX12UIBridge: Failed to create UI fence, HRESULT=" + std::to_string(hr));
                        return nullptr;
                    }
                    m_uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    if (m_uiFenceEvent == nullptr)
                    {
                        NLS_LOG_ERROR("DX12UIBridge: Failed to create UI fence event");
                        m_uiFence.Reset();
                        return nullptr;
                    }
                }
                return reinterpret_cast<void*>(m_uiFence.Get());
            }

            void SubmitCommandBuffer(uint32_t) override
            {
                // UI command buffer synchronization is handled by Driver via semaphores
            }

        private:
            bool EnsureSwapchainRenderResources(const NativeRenderDeviceInfo& nativeInfo)
            {
                if (!m_device || !m_queue)
                    return false;

                auto* swapchain = static_cast<IDXGISwapChain3*>(nativeInfo.swapchain);

                // If swapchain is not yet available, check if we already have resources
                if (swapchain == nullptr)
                {
                    // If we already have valid backbuffers and command list, we're good
                    if (m_commandList != nullptr && !m_backBuffers.empty() && !m_commandAllocators.empty())
                        return true;
                    // Otherwise, we can't proceed without a swapchain
                    return false;
                }

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

                if (!m_device || !m_queue)
                {
                    NLS_LOG_WARNING("DX12UIBridge initialization skipped because device or queue is missing.");
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

                // Initialize ImGui with default image count, swapchain resources will be created lazily
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

                // Try to create swapchain resources if swapchain is available
                // If not available, resources will be created lazily in RenderDrawData
                if (m_swapchain != nullptr)
                {
                    if (!EnsureSwapchainRenderResources(nativeInfo))
                    {
                        NLS_LOG_WARNING("DX12UIBridge failed to create swapchain resources, will retry later.");
                        // Don't fail init, we'll try again in RenderDrawData
                    }
                }

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

            std::unordered_map<uintptr_t, CachedTextureHandle> m_textureHandles;
            HANDLE m_fenceEvent = nullptr;
            UINT64 m_fenceValue = 0;
            UINT m_rtvDescriptorSize = 0;
            UINT m_srvDescriptorSize = 0;
            UINT m_srvDescriptorCapacity = 0;
            UINT m_nextTextureDescriptorIndex = 1;
            DXGI_FORMAT m_backbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            uint32_t m_imageCount = 0;
            bool m_initialized = false;
            // UI synchronization
            void* m_waitSemaphore = nullptr;
            void* m_signalSemaphore = nullptr;
            Microsoft::WRL::ComPtr<ID3D12Fence> m_uiFence;
            HANDLE m_uiFenceEvent = nullptr;
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
                ReleaseTextureHandles();
                DestroySwapchainFramebuffers();
                DestroyTextureSampler();

                if (m_initialized)
                {
                    ImGui_ImplVulkan_Shutdown();
                }
                DestroyUISemaphore();
            }

            bool HasRendererBackend() const override { return m_initialized; }

            void BeginFrame() override
            {
                if (!m_initialized)
                    return;

                ImGuiIO& io = ImGui::GetIO();

                // Ensure font atlas is built before NewFrame - fonts may have been loaded after Init
                if (!io.Fonts->IsBuilt())
                {
                    if (!io.Fonts->Build())
                    {
                        NLS_LOG_ERROR("VulkanUIBridge::BeginFrame: io.Fonts->Build() failed");
                        m_initialized = false;
                        return;
                    }
                }

                if (const auto* driver = ResolveUIDriver(); driver != nullptr)
                {
                    const auto nativeInfo = NLS::Render::Context::DriverUIAccess::GetNativeDeviceInfo(*driver);
                    const uint32_t imageCount = nativeInfo.swapchainImageCount > 0 ? nativeInfo.swapchainImageCount : 2u;
                    if (m_imageCount != imageCount)
                    {
                        ImGui_ImplVulkan_SetMinImageCount(imageCount);
                        m_imageCount = imageCount;
                    }
                }

                ImGui_ImplVulkan_NewFrame();
            }

            void NotifySwapchainWillResize() override
            {
                DestroySwapchainFramebuffers();
            }

            void RenderDrawData(ImDrawData* drawData, uint32_t) override
            {
                if (!m_initialized || drawData == nullptr)
                    return;

                auto* driver = ResolveUIDriver();
                if (driver == nullptr)
                    return;

                if (!NLS::Render::Context::DriverUIAccess::PrepareUIRender(*driver))
                    return;

                // Get the active command buffer directly from Driver
                auto commandBufferRHI = NLS::Render::Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(*driver);
                if (commandBufferRHI == nullptr)
                    return;

                auto swapchainBackbufferView = NLS::Render::Context::DriverRendererAccess::GetSwapchainBackbufferView(*driver);
                if (swapchainBackbufferView == nullptr)
                    return;

                void* nativeCmdBuffer = commandBufferRHI->GetNativeCommandBuffer();
                const auto commandBuffer = reinterpret_cast<VkCommandBuffer>(nativeCmdBuffer);
                if (commandBuffer == VK_NULL_HANDLE)
                    return;

                const int fbWidth = static_cast<int>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
                const int fbHeight = static_cast<int>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
                if (fbWidth <= 0 || fbHeight <= 0)
                    return;

                if (!BeginSwapchainRenderPass(
                    commandBuffer,
                    swapchainBackbufferView,
                    static_cast<uint32_t>(fbWidth),
                    static_cast<uint32_t>(fbHeight)))
                {
                    return;
                }

                ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
                EndSwapchainRenderPass(commandBuffer, swapchainBackbufferView);
            }

            NativeHandle ResolveTextureView(const std::shared_ptr<RHITextureView>& textureView) override
            {
                if (!m_initialized || textureView == nullptr)
                    return {};

                if (!EnsureTextureSampler())
                    return {};

                NLS::Render::RHI::NativeHandle nativeViewHandle = textureView->GetNativeShaderResourceView();
                if (!nativeViewHandle.IsValid() || nativeViewHandle.backend != NLS::Render::RHI::BackendType::Vulkan)
                    return {};

                const auto imageView = reinterpret_cast<VkImageView>(nativeViewHandle.handle);
                if (imageView == VK_NULL_HANDLE)
                    return {};

                const uintptr_t viewKey = reinterpret_cast<uintptr_t>(imageView);
                auto found = m_textureHandles.find(viewKey);
                if (found == m_textureHandles.end())
                {
                    const VkDescriptorSet descriptorSet = ImGui_ImplVulkan_AddTexture(
                        m_textureSampler,
                        imageView,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    if (descriptorSet == VK_NULL_HANDLE)
                    {
                        NLS_LOG_WARNING("VulkanUIBridge::ResolveTextureView: ImGui_ImplVulkan_AddTexture returned VK_NULL_HANDLE");
                        return {};
                    }

                    found = m_textureHandles.emplace(viewKey, descriptorSet).first;
                }

                return {
                    NLS::Render::RHI::BackendType::Vulkan,
                    reinterpret_cast<void*>(found->second)
                };
            }

            void SetWaitSemaphore(void* semaphore) override
            {
                m_waitSemaphore = semaphore;
            }

            void SetSignalSemaphore(void* semaphore) override
            {
                m_signalSemaphore = semaphore;
            }

            void* GetUISignalSemaphore() override
            {
                if (!m_initialized)
                    return nullptr;

                // Create Vulkan semaphore for UI signal if not already created
                if (m_uiSemaphore == VK_NULL_HANDLE)
                {
                    VkSemaphoreCreateInfo semaphoreInfo{};
                    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                    semaphoreInfo.flags = 0;

                    VkResult result = vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_uiSemaphore);
                    if (result != VK_SUCCESS)
                    {
                        NLS_LOG_ERROR("VulkanUIBridge: Failed to create UI semaphore, VkResult=" + std::to_string(static_cast<int>(result)));
                        return nullptr;
                    }
                }
                return reinterpret_cast<void*>(m_uiSemaphore);
            }

            void SubmitCommandBuffer(uint32_t) override
            {
                // UI rendering is submitted via RenderDrawData which uses the command buffer
                // from DriverUIAccess::GetNativeDeviceInfo().currentCommandBuffer
                // The synchronization (wait/signal semaphores) is handled by the Driver via
                // the semaphores set by SetWaitSemaphore/SetSignalSemaphore.
                // For proper synchronization, the wait semaphore should be waited on before
                // the UI command buffer executes, and signal semaphore should be signaled
                // when UI rendering is complete.
            }

        private:
            struct CachedFramebuffer
            {
                VkFramebuffer framebuffer = VK_NULL_HANDLE;
                uint32_t width = 0;
                uint32_t height = 0;
            };

            struct ImageLayoutState
            {
                VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
                bool initialized = false;
            };

            void ReleaseTextureHandles()
            {
                if (m_initialized)
                {
                    for (const auto& [_, descriptorSet] : m_textureHandles)
                    {
                        if (descriptorSet != VK_NULL_HANDLE)
                            ImGui_ImplVulkan_RemoveTexture(descriptorSet);
                    }
                }

                m_textureHandles.clear();
            }

            bool EnsureTextureSampler()
            {
                if (m_textureSampler != VK_NULL_HANDLE)
                    return true;

                if (m_device == VK_NULL_HANDLE)
                    return false;

                VkSamplerCreateInfo samplerInfo{};
                samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                samplerInfo.magFilter = VK_FILTER_LINEAR;
                samplerInfo.minFilter = VK_FILTER_LINEAR;
                samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

                const VkResult result = vkCreateSampler(m_device, &samplerInfo, nullptr, &m_textureSampler);
                if (result != VK_SUCCESS)
                {
                    NLS_LOG_ERROR(
                        "VulkanUIBridge::EnsureTextureSampler: vkCreateSampler failed, VkResult=" +
                        std::to_string(static_cast<int>(result)));
                    return false;
                }

                return true;
            }

            void DestroyTextureSampler()
            {
                if (m_device != VK_NULL_HANDLE && m_textureSampler != VK_NULL_HANDLE)
                {
                    vkDestroySampler(m_device, m_textureSampler, nullptr);
                    m_textureSampler = VK_NULL_HANDLE;
                }
            }

            void DestroySwapchainFramebuffers()
            {
                if (m_device != VK_NULL_HANDLE)
                {
                    for (auto& [_, cachedFramebuffer] : m_swapchainFramebuffers)
                    {
                        if (cachedFramebuffer.framebuffer != VK_NULL_HANDLE)
                            vkDestroyFramebuffer(m_device, cachedFramebuffer.framebuffer, nullptr);
                    }
                }

                m_swapchainFramebuffers.clear();
                m_swapchainImageStates.clear();
            }

            VkFramebuffer EnsureSwapchainFramebuffer(const VkImageView imageView, const uint32_t width, const uint32_t height)
            {
                if (m_device == VK_NULL_HANDLE || m_renderPass == VK_NULL_HANDLE || imageView == VK_NULL_HANDLE)
                    return VK_NULL_HANDLE;

                const uintptr_t framebufferKey = reinterpret_cast<uintptr_t>(imageView);
                auto& cachedFramebuffer = m_swapchainFramebuffers[framebufferKey];
                if (cachedFramebuffer.framebuffer != VK_NULL_HANDLE &&
                    cachedFramebuffer.width == width &&
                    cachedFramebuffer.height == height)
                {
                    return cachedFramebuffer.framebuffer;
                }

                if (cachedFramebuffer.framebuffer != VK_NULL_HANDLE)
                {
                    vkDestroyFramebuffer(m_device, cachedFramebuffer.framebuffer, nullptr);
                    cachedFramebuffer.framebuffer = VK_NULL_HANDLE;
                }

                VkFramebufferCreateInfo framebufferInfo{};
                framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                framebufferInfo.renderPass = m_renderPass;
                framebufferInfo.attachmentCount = 1;
                framebufferInfo.pAttachments = &imageView;
                framebufferInfo.width = width;
                framebufferInfo.height = height;
                framebufferInfo.layers = 1;

                if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &cachedFramebuffer.framebuffer) != VK_SUCCESS)
                {
                    NLS_LOG_WARNING("VulkanUIBridge::EnsureSwapchainFramebuffer: failed to create framebuffer");
                    return VK_NULL_HANDLE;
                }

                cachedFramebuffer.width = width;
                cachedFramebuffer.height = height;
                return cachedFramebuffer.framebuffer;
            }

            bool ResolveSwapchainTargets(
                const std::shared_ptr<RHITextureView>& swapchainBackbufferView,
                VkImage& image,
                VkImageView& imageView) const
            {
                if (swapchainBackbufferView == nullptr)
                    return false;

                const auto nativeImageView = swapchainBackbufferView->GetNativeRenderTargetView();
                if (!nativeImageView.IsValid() || nativeImageView.backend != NLS::Render::RHI::BackendType::Vulkan)
                    return false;

                const auto& backbufferTexture = swapchainBackbufferView->GetTexture();
                if (backbufferTexture == nullptr)
                    return false;

                const auto nativeImage = backbufferTexture->GetNativeImageHandle();
                if (!nativeImage.IsValid() || nativeImage.backend != NLS::Render::RHI::BackendType::Vulkan)
                    return false;

                image = reinterpret_cast<VkImage>(nativeImage.handle);
                imageView = reinterpret_cast<VkImageView>(nativeImageView.handle);
                return image != VK_NULL_HANDLE && imageView != VK_NULL_HANDLE;
            }

            bool BeginSwapchainRenderPass(
                const VkCommandBuffer commandBuffer,
                const std::shared_ptr<RHITextureView>& swapchainBackbufferView,
                const uint32_t width,
                const uint32_t height)
            {
                VkImage image = VK_NULL_HANDLE;
                VkImageView imageView = VK_NULL_HANDLE;
                if (!ResolveSwapchainTargets(swapchainBackbufferView, image, imageView))
                    return false;

                const VkFramebuffer framebuffer = EnsureSwapchainFramebuffer(imageView, width, height);
                if (framebuffer == VK_NULL_HANDLE)
                    return false;

                auto& imageState = m_swapchainImageStates[reinterpret_cast<uintptr_t>(image)];

                VkImageMemoryBarrier transitionToColorAttachment{};
                transitionToColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                transitionToColorAttachment.srcAccessMask = imageState.initialized && imageState.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                    ? VK_ACCESS_MEMORY_READ_BIT
                    : 0;
                transitionToColorAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                transitionToColorAttachment.oldLayout = imageState.initialized
                    ? imageState.layout
                    : VK_IMAGE_LAYOUT_UNDEFINED;
                transitionToColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                transitionToColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                transitionToColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                transitionToColorAttachment.image = image;
                transitionToColorAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                transitionToColorAttachment.subresourceRange.baseMipLevel = 0;
                transitionToColorAttachment.subresourceRange.levelCount = 1;
                transitionToColorAttachment.subresourceRange.baseArrayLayer = 0;
                transitionToColorAttachment.subresourceRange.layerCount = 1;

                vkCmdPipelineBarrier(
                    commandBuffer,
                    imageState.initialized ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    0,
                    0,
                    nullptr,
                    0,
                    nullptr,
                    1,
                    &transitionToColorAttachment);

                VkRenderPassBeginInfo renderPassBeginInfo{};
                renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                renderPassBeginInfo.renderPass = m_renderPass;
                renderPassBeginInfo.framebuffer = framebuffer;
                renderPassBeginInfo.renderArea.offset = {0, 0};
                renderPassBeginInfo.renderArea.extent = {width, height};

                vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

                imageState.initialized = true;
                imageState.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                return true;
            }

            void EndSwapchainRenderPass(
                const VkCommandBuffer commandBuffer,
                const std::shared_ptr<RHITextureView>& swapchainBackbufferView)
            {
                VkImage image = VK_NULL_HANDLE;
                VkImageView imageView = VK_NULL_HANDLE;
                if (!ResolveSwapchainTargets(swapchainBackbufferView, image, imageView))
                    return;

                vkCmdEndRenderPass(commandBuffer);

                auto& imageState = m_swapchainImageStates[reinterpret_cast<uintptr_t>(image)];

                VkImageMemoryBarrier transitionToPresent{};
                transitionToPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                transitionToPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                transitionToPresent.dstAccessMask = 0;
                transitionToPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                transitionToPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                transitionToPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                transitionToPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                transitionToPresent.image = image;
                transitionToPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                transitionToPresent.subresourceRange.baseMipLevel = 0;
                transitionToPresent.subresourceRange.levelCount = 1;
                transitionToPresent.subresourceRange.baseArrayLayer = 0;
                transitionToPresent.subresourceRange.layerCount = 1;

                vkCmdPipelineBarrier(
                    commandBuffer,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    0,
                    0,
                    nullptr,
                    0,
                    nullptr,
                    1,
                    &transitionToPresent);

                imageState.initialized = true;
                imageState.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            }

            void DestroyUISemaphore()
            {
                if (m_device != VK_NULL_HANDLE && m_uiSemaphore != VK_NULL_HANDLE)
                {
                    vkDestroySemaphore(m_device, m_uiSemaphore, nullptr);
                    m_uiSemaphore = VK_NULL_HANDLE;
                }
            }

            static void CheckVulkanResult(const VkResult result)
            {
                if (result != VK_SUCCESS)
                    NLS_LOG_ERROR("ImGui Vulkan backend reported VkResult=" + std::to_string(static_cast<int>(result)));
            }

            bool Initialize(const NativeRenderDeviceInfo& nativeInfo)
            {
                const auto instance = reinterpret_cast<VkInstance>(nativeInfo.instance);
                const auto physicalDevice = reinterpret_cast<VkPhysicalDevice>(nativeInfo.physicalDevice);
                m_device = reinterpret_cast<VkDevice>(nativeInfo.device);
                const auto queue = reinterpret_cast<VkQueue>(nativeInfo.graphicsQueue);
                m_renderPass = reinterpret_cast<VkRenderPass>(nativeInfo.uiRenderPass);
                const auto descriptorPool = reinterpret_cast<VkDescriptorPool>(nativeInfo.uiDescriptorPool);
                const uint32_t imageCount = nativeInfo.swapchainImageCount > 0 ? nativeInfo.swapchainImageCount : 2u;

                NLS_LOG_INFO("VulkanUIBridge::Initialize: instance=" + std::to_string(reinterpret_cast<uintptr_t>(instance)) +
                    " physicalDevice=" + std::to_string(reinterpret_cast<uintptr_t>(physicalDevice)) +
                    " device=" + std::to_string(reinterpret_cast<uintptr_t>(m_device)) +
                    " queue=" + std::to_string(reinterpret_cast<uintptr_t>(queue)) +
                    " renderPass=" + std::to_string(reinterpret_cast<uintptr_t>(m_renderPass)) +
                    " descriptorPool=" + std::to_string(reinterpret_cast<uintptr_t>(descriptorPool)) +
                    " imageCount=" + std::to_string(imageCount));

                // The explicit Vulkan path must provide a compatible render pass because the UI bridge
                // begins and ends the swapchain render pass around ImGui_ImplVulkan_RenderDrawData().
                if (instance == VK_NULL_HANDLE ||
                    physicalDevice == VK_NULL_HANDLE ||
                    m_device == VK_NULL_HANDLE ||
                    queue == VK_NULL_HANDLE ||
                    m_renderPass == VK_NULL_HANDLE ||
                    descriptorPool == VK_NULL_HANDLE)
                {
                    NLS_LOG_WARNING("VulkanUIBridge::Initialize: one or more required handles are null, skipping initialization");
                    return false;
                }

                ImGui_ImplVulkan_InitInfo initInfo{};
                initInfo.Instance = instance;
                initInfo.PhysicalDevice = physicalDevice;
                initInfo.Device = m_device;
                initInfo.QueueFamily = nativeInfo.graphicsQueueFamilyIndex;
                initInfo.Queue = queue;
                initInfo.DescriptorPool = descriptorPool;
                initInfo.RenderPass = m_renderPass;
                initInfo.MinImageCount = imageCount;
                initInfo.ImageCount = imageCount;
                initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
                initInfo.MinAllocationSize = 1024 * 1024;
                initInfo.CheckVkResultFn = CheckVulkanResult;

                NLS_LOG_INFO("VulkanUIBridge::Initialize: calling ImGui_ImplVulkan_Init with renderPass=" +
                    std::to_string(reinterpret_cast<uintptr_t>(m_renderPass)));
                if (!ImGui_ImplVulkan_Init(&initInfo))
                {
                    NLS_LOG_WARNING("VulkanUIBridge::Initialize: ImGui_ImplVulkan_Init failed");
                    return false;
                }
                NLS_LOG_INFO("VulkanUIBridge::Initialize: ImGui_ImplVulkan_Init succeeded");

                m_imageCount = imageCount;
                m_uiSemaphore = VK_NULL_HANDLE;
                m_initialized = true;
                return true;
            }

        private:
            uint32_t m_imageCount = 0;
            bool m_initialized = false;
            VkDevice m_device = VK_NULL_HANDLE;
            VkRenderPass m_renderPass = VK_NULL_HANDLE;
            VkSemaphore m_uiSemaphore = VK_NULL_HANDLE;
            VkSampler m_textureSampler = VK_NULL_HANDLE;
            void* m_waitSemaphore = nullptr;
            void* m_signalSemaphore = nullptr;
            std::unordered_map<uintptr_t, VkDescriptorSet> m_textureHandles;
            std::unordered_map<uintptr_t, CachedFramebuffer> m_swapchainFramebuffers;
            std::unordered_map<uintptr_t, ImageLayoutState> m_swapchainImageStates;
        };
#endif
    }

    std::unique_ptr<RHIUIBridge> CreateRHIUIBridge(
        GLFWwindow*,
        NLS::Render::Settings::EGraphicsBackend backend,
        const std::string& glslVersion,
        const NativeRenderDeviceInfo* nativeDeviceInfo)
    {
        NLS_LOG_INFO("CreateRHIUIBridge: requested backend=" + std::to_string(static_cast<int>(backend)));

        if (!NLS::Render::Settings::SupportsImGuiRendererBackend(backend))
            return std::make_unique<NullUIBridge>();

        // Use provided native device info if available, otherwise resolve from driver
        NativeRenderDeviceInfo resolvedNativeInfo;
        if (nativeDeviceInfo != nullptr)
        {
            resolvedNativeInfo = *nativeDeviceInfo;
            NLS_LOG_INFO("CreateRHIUIBridge: using provided native device info (device=" +
                std::to_string(reinterpret_cast<uintptr_t>(resolvedNativeInfo.device)) +
                ", swapchain=" + std::to_string(reinterpret_cast<uintptr_t>(resolvedNativeInfo.swapchain)) + ")");
        }
        else
        {
            resolvedNativeInfo = ResolveNativeDeviceInfo(backend);
            NLS_LOG_INFO("CreateRHIUIBridge: resolved native device info from driver");
        }

        switch (backend)
        {
        case NLS::Render::Settings::EGraphicsBackend::OPENGL:
            return std::make_unique<OpenGLUIBridge>(glslVersion);
#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND
        case NLS::Render::Settings::EGraphicsBackend::DX12:
            return std::make_unique<DX12UIBridge>(resolvedNativeInfo);
#endif
#if NLS_HAS_VULKAN && NLS_HAS_IMGUI_VULKAN_BACKEND
        case NLS::Render::Settings::EGraphicsBackend::VULKAN:
            return std::make_unique<VulkanUIBridge>(resolvedNativeInfo);
#endif
        case NLS::Render::Settings::EGraphicsBackend::DX11:
        case NLS::Render::Settings::EGraphicsBackend::METAL:
        case NLS::Render::Settings::EGraphicsBackend::NONE:
        default:
            return std::make_unique<NullUIBridge>();
        }
    }
}
