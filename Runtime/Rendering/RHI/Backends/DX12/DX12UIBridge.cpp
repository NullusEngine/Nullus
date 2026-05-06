#include "Rendering/RHI/Utils/RHIUIBridgeInternal.h"

#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <unordered_map>
#include <vector>

#include "Debug/Logger.h"
#include "Profiling/Profiler.h"
#include "Rendering/RHI/Backends/DX12/DX12UIFrameFenceTracker.h"
#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "ImGui/backends/imgui_impl_dx12.h"

namespace NLS::Render::RHI
{
    namespace
    {
        bool ShouldLogDx12FrameFlow()
        {
            return NLS::Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow;
        }

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

            NativeBackendType GetNativeBackendType() const override { return NativeBackendType::DX12; }
            bool HasRendererBackend() const override { return m_initialized; }

            void BeginFrame() override
            {
                if (!m_initialized)
                    return;

                ImGuiIO& io = ImGui::GetIO();
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
                NLS_PROFILE_SCOPE();
                if (!m_initialized || drawData == nullptr)
                    return;

                if (ShouldLogDx12FrameFlow())
                    NLS_LOG_INFO("DX12UIBridge::RenderDrawData: begin");

                auto* driver = ResolveUIDriver();
                if (driver == nullptr)
                {
                    if (ShouldLogDx12FrameFlow())
                        NLS_LOG_INFO("DX12UIBridge::RenderDrawData: skipped because UI driver is unavailable");
                    return;
                }

                if (!NLS::Render::Context::DriverUIAccess::PrepareUIRender(*driver))
                {
                    if (ShouldLogDx12FrameFlow())
                        NLS_LOG_INFO("DX12UIBridge::RenderDrawData: skipped because PrepareUIRender returned false");
                    return;
                }

                const auto nativeInfo = NLS::Render::Context::DriverUIAccess::GetNativeDeviceInfo(*driver);
                if (!EnsureSwapchainRenderResources(nativeInfo))
                {
                    if (ShouldLogDx12FrameFlow())
                        NLS_LOG_INFO("DX12UIBridge::RenderDrawData: skipped because swapchain render resources are unavailable");
                    return;
                }

                const UINT backBufferIndex = m_swapchain->GetCurrentBackBufferIndex();
                if (backBufferIndex >= m_commandAllocators.size() || backBufferIndex >= m_backBuffers.size())
                {
                    NLS_LOG_ERROR(
                        "DX12UIBridge::RenderDrawData: swapchain backbuffer index is outside UI frame resources.");
                    return;
                }

                if (ShouldLogDx12FrameFlow())
                {
                    NLS_LOG_INFO(
                        "DX12UIBridge::RenderDrawData: recording UI commands backBufferIndex=" +
                        std::to_string(backBufferIndex));
                }

                auto& commandAllocator = m_commandAllocators[backBufferIndex];
                auto& backBuffer = m_backBuffers[backBufferIndex];

                const auto reuseWait = m_frameFenceTracker.ResolveReuseWait(
                    backBufferIndex,
                    m_fence->GetCompletedValue());
                if (reuseWait.shouldWait)
                {
                    NLS_PROFILE_NAMED_SCOPE("DX12UIBridge::WaitForBackbufferReuse");
                    m_fence->SetEventOnCompletion(reuseWait.fenceValue, m_fenceEvent);
                    WaitForSingleObject(m_fenceEvent, INFINITE);
                }

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

                const FLOAT clearColor[] = { 0.06f, 0.05f, 0.07f, 1.0f };
                m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

                ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
                m_commandList->SetDescriptorHeaps(1, heaps);
                {
                    NLS_PROFILE_NAMED_SCOPE("ImGui_ImplDX12_RenderDrawData");
                    ImGui_ImplDX12_RenderDrawData(drawData, m_commandList.Get());
                }
                if (ShouldLogDx12FrameFlow())
                    NLS_LOG_INFO("DX12UIBridge::RenderDrawData: ImGui draw data recorded");

                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                m_commandList->ResourceBarrier(1, &barrier);
                m_commandList->Close();

                ID3D12CommandList* commandLists[] = { m_commandList.Get() };
                {
                    NLS_PROFILE_NAMED_SCOPE("DX12UIBridge::ExecuteCommandLists");
                    m_queue->ExecuteCommandLists(1, commandLists);
                }
                if (ShouldLogDx12FrameFlow())
                    NLS_LOG_INFO("DX12UIBridge::RenderDrawData: UI command list submitted");

                const UINT64 fenceValue = ++m_fenceValue;
                m_queue->Signal(m_fence.Get(), fenceValue);
                m_frameFenceTracker.RecordSubmitted(backBufferIndex, fenceValue);

                if (m_uiFence != nullptr)
                {
                    m_queue->Signal(m_uiFence.Get(), fenceValue);
                }

                if (ShouldLogDx12FrameFlow())
                {
                    NLS_LOG_INFO(
                        "DX12UIBridge::RenderDrawData: UI submitted fenceValue=" +
                        std::to_string(fenceValue));
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
                if (found == m_textureHandles.end() || found->second.resource != resource)
                {
                    const bool reuseExistingSlot = found != m_textureHandles.end();
                    if (!reuseExistingSlot && m_nextTextureDescriptorIndex >= m_srvDescriptorCapacity)
                    {
                        NLS_LOG_WARNING("DX12UIBridge::ResolveTextureView: SRV heap capacity exhausted.");
                        return {};
                    }

                    const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(
                        texture->GetDesc(),
                        textureView->GetDesc());
                    if (!descriptors.hasSrv)
                        return {};

                    const UINT descriptorIndex = reuseExistingSlot
                        ? found->second.descriptorIndex
                        : m_nextTextureDescriptorIndex;

                    D3D12_CPU_DESCRIPTOR_HANDLE destinationCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                    destinationCpu.ptr += static_cast<SIZE_T>(descriptorIndex) * m_srvDescriptorSize;

                    D3D12_GPU_DESCRIPTOR_HANDLE destinationGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
                    destinationGpu.ptr += static_cast<UINT64>(descriptorIndex) * m_srvDescriptorSize;

                    m_device->CreateShaderResourceView(resource, &descriptors.srvDesc, destinationCpu);

                    CachedTextureHandle cachedHandle;
                    cachedHandle.resource = resource;
                    cachedHandle.gpuHandle = destinationGpu;
                    cachedHandle.descriptorIndex = descriptorIndex;
                    if (!reuseExistingSlot)
                        ++m_nextTextureDescriptorIndex;

                    if (reuseExistingSlot)
                        found->second = cachedHandle;
                    else
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

            uint64_t GetUISignalValue() const override
            {
                return m_frameFenceTracker.GetLastSubmittedFenceValue();
            }

            void SubmitCommandBuffer(uint32_t) override
            {
            }

        private:
            struct CachedTextureHandle
            {
                ID3D12Resource* resource = nullptr;
                D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
                UINT descriptorIndex = 0;
            };

            bool EnsureSwapchainRenderResources(const NativeRenderDeviceInfo& nativeInfo)
            {
                if (!m_device || !m_queue)
                    return false;

                auto* swapchain = static_cast<IDXGISwapChain3*>(nativeInfo.swapchain);
                if (swapchain == nullptr)
                {
                    if (m_commandList != nullptr && !m_backBuffers.empty() && !m_commandAllocators.empty())
                        return true;
                    return false;
                }

                m_swapchain = swapchain;

                DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
                if (FAILED(swapchain->GetDesc1(&swapchainDesc)))
                {
                    NLS_LOG_WARNING("DX12UIBridge failed to query swapchain desc.");
                    return false;
                }

                const uint32_t targetImageCount = swapchainDesc.BufferCount != 0
                    ? swapchainDesc.BufferCount
                    : (nativeInfo.swapchainImageCount > 0 ? nativeInfo.swapchainImageCount : 2u);
                const uint32_t targetWidth = swapchainDesc.Width;
                const uint32_t targetHeight = swapchainDesc.Height;
                const DXGI_FORMAT targetFormat = swapchainDesc.Format == DXGI_FORMAT_UNKNOWN
                    ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : swapchainDesc.Format;
                if (m_commandList != nullptr &&
                    m_swapchain.Get() == swapchain &&
                    !m_backBuffers.empty() &&
                    m_backBuffers.size() == targetImageCount &&
                    m_commandAllocators.size() == targetImageCount &&
                    m_backbufferWidth == targetWidth &&
                    m_backbufferHeight == targetHeight &&
                    m_backbufferFormat == targetFormat)
                {
                    return true;
                }

                ReleaseSwapchainRenderResources();
                m_swapchain = swapchain;

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
                m_backbufferWidth = targetWidth;
                m_backbufferHeight = targetHeight;
                m_backbufferFormat = targetFormat;
                m_frameFenceTracker.ResetBackbufferCount(m_imageCount);
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
                m_frameFenceTracker.ResetBackbufferCount(0u);
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

                if (m_swapchain != nullptr)
                {
                    if (!EnsureSwapchainRenderResources(nativeInfo))
                    {
                        NLS_LOG_WARNING("DX12UIBridge failed to create swapchain resources, will retry later.");
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

                ReleaseSwapchainRenderResources();
                m_textureHandles.clear();
                m_srvHeap.Reset();
                m_uiFence.Reset();
                if (m_uiFenceEvent != nullptr)
                {
                    CloseHandle(m_uiFenceEvent);
                    m_uiFenceEvent = nullptr;
                }
                m_fence.Reset();

                if (m_fenceEvent != nullptr)
                {
                    CloseHandle(m_fenceEvent);
                    m_fenceEvent = nullptr;
                }

                m_swapchain.Reset();
                m_queue.Reset();
                m_device.Reset();
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
            std::unordered_map<uintptr_t, CachedTextureHandle> m_textureHandles;
            HANDLE m_fenceEvent = nullptr;
            UINT64 m_fenceValue = 0;
            UINT m_rtvDescriptorSize = 0;
            UINT m_srvDescriptorSize = 0;
            UINT m_srvDescriptorCapacity = 0;
            UINT m_nextTextureDescriptorIndex = 1;
            DXGI_FORMAT m_backbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            uint32_t m_backbufferWidth = 0;
            uint32_t m_backbufferHeight = 0;
            uint32_t m_imageCount = 0;
            bool m_initialized = false;
            void* m_waitSemaphore = nullptr;
            void* m_signalSemaphore = nullptr;
            Microsoft::WRL::ComPtr<ID3D12Fence> m_uiFence;
            HANDLE m_uiFenceEvent = nullptr;
            NLS::Render::RHI::DX12::DX12UIFrameFenceTracker m_frameFenceTracker;
        };
    }

    std::unique_ptr<RHIUIBridge> CreateDX12RHIUIBridge(const NativeRenderDeviceInfo& nativeInfo)
    {
        return std::make_unique<DX12UIBridge>(nativeInfo);
    }
}

#endif
