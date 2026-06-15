#include "Rendering/RHI/Utils/RHIUIBridgeInternal.h"
#include "ImGui/imgui.h"

#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <vector>

#include "Debug/Logger.h"
#include "Profiling/Profiler.h"
#include "Rendering/RHI/Backends/DX12/DX12InfoQueueUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12QueueSynchronization.h"
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

        bool WaitForDX12UIFence(
            ID3D12Fence* fence,
            HANDLE fenceEvent,
            UINT64 fenceValue,
            const std::string& context)
        {
            if (fence == nullptr || fenceEvent == nullptr || fenceValue == 0u)
                return false;
            if (fence->GetCompletedValue() >= fenceValue)
                return true;

            const HRESULT setEventHr = fence->SetEventOnCompletion(fenceValue, fenceEvent);
            if (FAILED(setEventHr))
            {
                NLS_LOG_ERROR(
                    context +
                    ": SetEventOnCompletion failed hr=" +
                    std::to_string(setEventHr) +
                    " value=" +
                    std::to_string(fenceValue));
                return false;
            }

            const DWORD waitResult = WaitForSingleObject(
                fenceEvent,
                DX12::GetDX12UIFenceWaitTimeoutMilliseconds());
            if (waitResult != WAIT_OBJECT_0)
            {
                NLS_LOG_ERROR(
                    context +
                    ": timed out waiting for UI fence value=" +
                    std::to_string(fenceValue) +
                    " completed=" +
                    std::to_string(fence->GetCompletedValue()));
                return false;
            }
            return true;
        }

        bool WaitForDX12QueueFence(
            ID3D12CommandQueue* queue,
            const NativeHandle& waitSemaphore,
            const std::string& context)
        {
            if (!waitSemaphore.IsValid())
                return true;

            if (waitSemaphore.backend != NLS::Render::RHI::BackendType::DX12 ||
                waitSemaphore.value == 0u)
            {
                NLS_LOG_ERROR(
                    context +
                    ": invalid scene wait semaphore backend=" +
                    std::to_string(static_cast<int>(waitSemaphore.backend)) +
                    " value=" +
                    std::to_string(waitSemaphore.value));
                return false;
            }

            auto* fence = reinterpret_cast<ID3D12Fence*>(waitSemaphore.handle);
            if (queue == nullptr || fence == nullptr)
                return false;

            const HRESULT waitHr = [&]()
            {
                DX12::ScopedDX12QueueLock queueLock(queue);
                return queue->Wait(fence, waitSemaphore.value);
            }();
            if (FAILED(waitHr))
            {
                NLS_LOG_ERROR(
                    context +
                    ": queue wait failed hr=" +
                    std::to_string(waitHr) +
                    " value=" +
                    std::to_string(waitSemaphore.value));
                return false;
            }

            return true;
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

                RetireCompletedTextureHandles();
                DiscardCurrentFrameTextureHandles();

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

            void ReleaseTextureViewHandle(const std::shared_ptr<RHITextureView>& textureView) override
            {
                if (textureView == nullptr)
                    return;

                if (!WaitForSubmittedUiWork())
                {
                    QuarantineAllUiBridgeResources("DX12UIBridge::ReleaseTextureViewHandle failed to drain submitted UI work");
                    NLS_LOG_ERROR(
                        "DX12UIBridge::ReleaseTextureViewHandle: preserving texture handle because submitted UI work could not be drained");
                    return;
                }
                RetireCompletedTextureHandles();
                const uintptr_t viewKey = reinterpret_cast<uintptr_t>(textureView.get());
                DiscardCurrentFrameTextureViewHandle(viewKey);
                RemoveRetiredTextureViewHandleBatches(viewKey);
                auto handleIt = m_textureHandles.find(viewKey);
                if (handleIt != m_textureHandles.end())
                {
                    if (handleIt->second.descriptorIndex != 0u)
                        ReleaseTextureDescriptorIndex(handleIt->second.descriptorIndex);
                    m_textureHandles.erase(handleIt);
                }
            }

            void RetireTextureViewHandle(const std::shared_ptr<RHITextureView>& textureView) override
            {
                if (textureView == nullptr)
                    return;

                RetireCompletedTextureHandles();
                const uintptr_t viewKey = reinterpret_cast<uintptr_t>(textureView.get());
                auto handleIt = m_textureHandles.find(viewKey);
                if (handleIt == m_textureHandles.end())
                    return;

                const UINT descriptorIndex = handleIt->second.descriptorIndex;
                const bool referencedByCurrentFrame =
                    IsTextureViewHandleReferencedByCurrentFrame(viewKey, descriptorIndex);
                const bool referencedBySubmittedWork =
                    m_textureDescriptorInFlightUseCounts.find(descriptorIndex) !=
                        m_textureDescriptorInFlightUseCounts.end();
                if (!referencedByCurrentFrame && !referencedBySubmittedWork)
                {
                    if (descriptorIndex != 0u)
                        ReleaseTextureDescriptorIndex(descriptorIndex);
                    m_textureHandles.erase(handleIt);
                    return;
                }

                if (referencedByCurrentFrame)
                {
                    m_currentFrameRetirementTracker.RetireCurrentFrameUse(
                        { viewKey, descriptorIndex },
                        true);
                    return;
                }

                RetiredTextureHandleBatch batch;
                batch.fenceValue = m_fenceValue;
                batch.textureHandleUses.push_back({ viewKey, descriptorIndex });
                batch.textureViews.push_back(textureView);
                ++m_textureDescriptorInFlightUseCounts[descriptorIndex];
                m_textureHandles.erase(handleIt);
                m_retiredTextureHandleBatches.push_back(std::move(batch));
            }

            void NotifyFontAtlasChanged() override
            {
                if (!m_initialized)
                    return;

                ImGui_ImplDX12_InvalidateDeviceObjects();
                ImGui_ImplDX12_CreateDeviceObjects();
            }

            void RenderDrawData(
                ImDrawData* drawData,
                uint32_t,
                const WaitSemaphoreResolver& resolveWaitSemaphore = {}) override
            {
                NLS_PROFILE_SCOPE();
                m_lastSubmittedUiSignalValue = 0u;
                if (!m_initialized || drawData == nullptr)
                {
                    DiscardCurrentFrameTextureHandles();
                    return;
                }

                if (ShouldLogDx12FrameFlow())
                    NLS_LOG_INFO("DX12UIBridge::RenderDrawData: begin");

                auto* driver = ResolveUIDriver();
                if (driver == nullptr)
                {
                    if (ShouldLogDx12FrameFlow())
                        NLS_LOG_INFO("DX12UIBridge::RenderDrawData: skipped because UI driver is unavailable");
                    DiscardCurrentFrameTextureHandles();
                    return;
                }

                if (!NLS::Render::Context::DriverUIAccess::PrepareUIRender(*driver))
                {
                    if (ShouldLogDx12FrameFlow())
                        NLS_LOG_INFO("DX12UIBridge::RenderDrawData: skipped because PrepareUIRender returned false");
                    DiscardCurrentFrameTextureHandles();
                    return;
                }

                const auto nativeInfo = NLS::Render::Context::DriverUIAccess::GetNativeDeviceInfo(*driver);
                if (!EnsureSwapchainRenderResources(nativeInfo))
                {
                    if (ShouldLogDx12FrameFlow())
                        NLS_LOG_INFO("DX12UIBridge::RenderDrawData: skipped because swapchain render resources are unavailable");
                    DiscardCurrentFrameTextureHandles();
                    return;
                }

                const NativeHandle sceneWaitSemaphore = resolveWaitSemaphore
                    ? resolveWaitSemaphore()
                    : m_waitSemaphore;
                m_waitSemaphore = {};

                const UINT backBufferIndex = m_swapchain->GetCurrentBackBufferIndex();
                if (backBufferIndex >= m_commandAllocators.size() || backBufferIndex >= m_backBuffers.size())
                {
                    NLS_LOG_ERROR(
                        "DX12UIBridge::RenderDrawData: swapchain backbuffer index is outside UI frame resources.");
                    DiscardCurrentFrameTextureHandles();
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
                    if (!WaitForDX12UIFence(
                        m_fence.Get(),
                        m_fenceEvent,
                        reuseWait.fenceValue,
                        "DX12UIBridge::RenderDrawData(backbuffer reuse)"))
                    {
                        DiscardCurrentFrameTextureHandles();
                        return;
                    }
                }

                if (!WaitForDX12QueueFence(
                    m_queue.Get(),
                    sceneWaitSemaphore,
                    "DX12UIBridge::RenderDrawData(scene wait)"))
                {
                    DiscardCurrentFrameTextureHandles();
                    return;
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
                {
                    const DX12::ScopedDx12InfoQueueMessageFilter backbufferClearValueFilter(
                        m_device.Get(),
                        D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE);
                    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                }

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

                m_lastSubmittedUiSignalValue = 0u;
                const UINT64 fenceValue = ++m_fenceValue;
                HRESULT frameSignalHr = S_OK;
                HRESULT uiSignalHr = S_OK;
                {
                    DX12::ScopedDX12QueueLock queueLock(m_queue.Get());
                    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
                    {
                        NLS_PROFILE_NAMED_SCOPE("DX12UIBridge::ExecuteCommandLists");
                        m_queue->ExecuteCommandLists(1, commandLists);
                    }
                    frameSignalHr = m_queue->Signal(m_fence.Get(), fenceValue);
                    if (SUCCEEDED(frameSignalHr) && m_uiFence != nullptr)
                        uiSignalHr = m_queue->Signal(m_uiFence.Get(), fenceValue);
                }
                if (m_device != nullptr)
                {
                    const HRESULT deviceStatus = m_device->GetDeviceRemovedReason();
                    if (FAILED(deviceStatus))
                    {
                        const std::string message =
                            "DX12UIBridge::RenderDrawData: device status after ExecuteCommandLists hr=" +
                            std::to_string(deviceStatus);
                        QuarantineSubmittedUiFrameAfterExecute(
                            backBufferIndex,
                            deviceStatus,
                            fenceValue,
                            "DX12UIBridge::RenderDrawData(device removed after execute)");
                        MarkDriverDeviceLost(message);
                        NLS_LOG_ERROR(message);
                        return;
                    }
                }
                if (ShouldLogDx12FrameFlow())
                    NLS_LOG_INFO("DX12UIBridge::RenderDrawData: UI command list submitted");

                if (FAILED(frameSignalHr))
                {
                    QuarantineSubmittedUiFrameAfterExecute(
                        backBufferIndex,
                        frameSignalHr,
                        fenceValue,
                        "DX12UIBridge::RenderDrawData(frame fence signal)");
                    NLS_LOG_ERROR(
                        "DX12UIBridge::RenderDrawData: UI frame fence signal failed hr=" +
                        std::to_string(frameSignalHr) +
                        " value=" +
                        std::to_string(fenceValue));
                    return;
                }
                m_frameFenceTracker.RecordSubmitted(backBufferIndex, fenceValue);
                RetainCurrentFrameTextureHandles(fenceValue);

                if (m_uiFence != nullptr)
                {
                    if (FAILED(uiSignalHr))
                    {
                        const HRESULT uiSignalDeviceStatus = m_device != nullptr
                            ? m_device->GetDeviceRemovedReason()
                            : S_OK;
                        const std::string message =
                            "DX12UIBridge::RenderDrawData: UI composition fence signal failed hr=" +
                            std::to_string(uiSignalHr) +
                            " value=" +
                            std::to_string(fenceValue);
                        if (FAILED(uiSignalDeviceStatus))
                        {
                            MarkDriverDeviceLost(
                                message +
                                "; device status hr=" +
                                std::to_string(uiSignalDeviceStatus));
                        }
                        NLS_LOG_ERROR(
                            message);
                        return;
                    }
                    m_lastSubmittedUiSignalValue = fenceValue;
                }

                RetireCompletedTextureHandles();

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
                    const bool hasExistingSlot = found != m_textureHandles.end();
                    const bool existingSlotInFlight =
                        hasExistingSlot &&
                        m_textureDescriptorInFlightUseCounts.find(found->second.descriptorIndex) !=
                            m_textureDescriptorInFlightUseCounts.end();
                    const bool existingSlotReferencedByCurrentFrame =
                        hasExistingSlot &&
                        IsTextureDescriptorReferencedByCurrentFrame(found->second.descriptorIndex);
                    const bool reuseExistingSlot =
                        hasExistingSlot &&
                        !existingSlotReferencedByCurrentFrame &&
                        (!existingSlotInFlight || found->second.resource == resource);
                    if (!reuseExistingSlot &&
                        m_freeTextureDescriptorIndices.empty() &&
                        m_nextTextureDescriptorIndex >= m_srvDescriptorCapacity)
                    {
                        NLS_LOG_WARNING("DX12UIBridge::ResolveTextureView: SRV heap capacity exhausted.");
                        return {};
                    }

                    const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(
                        texture->GetDesc(),
                        textureView->GetDesc());
                    if (!descriptors.hasSrv)
                        return {};

                    UINT descriptorIndex = 0;
                    if (reuseExistingSlot)
                    {
                        descriptorIndex = found->second.descriptorIndex;
                    }
                    else if (!m_freeTextureDescriptorIndices.empty())
                    {
                        descriptorIndex = m_freeTextureDescriptorIndices.back();
                        m_freeTextureDescriptorIndices.pop_back();
                    }
                    else
                    {
                        descriptorIndex = m_nextTextureDescriptorIndex;
                    }

                    D3D12_CPU_DESCRIPTOR_HANDLE destinationCpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
                    destinationCpu.ptr += static_cast<SIZE_T>(descriptorIndex) * m_srvDescriptorSize;

                    D3D12_GPU_DESCRIPTOR_HANDLE destinationGpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
                    destinationGpu.ptr += static_cast<UINT64>(descriptorIndex) * m_srvDescriptorSize;

                    m_device->CreateShaderResourceView(resource, &descriptors.srvDesc, destinationCpu);

                    CachedTextureHandle cachedHandle;
                    cachedHandle.resource = resource;
                    cachedHandle.gpuHandle = destinationGpu;
                    cachedHandle.descriptorIndex = descriptorIndex;
                    if (!reuseExistingSlot && descriptorIndex == m_nextTextureDescriptorIndex)
                        ++m_nextTextureDescriptorIndex;

                    if (reuseExistingSlot)
                        found->second = cachedHandle;
                    else if (hasExistingSlot)
                        found->second = cachedHandle;
                    else
                        found = m_textureHandles.emplace(viewKey, cachedHandle).first;
                }

                KeepTextureViewForCurrentFrame(viewKey, found->second.descriptorIndex, textureView);
                return {
                    NLS::Render::RHI::BackendType::DX12,
                    reinterpret_cast<void*>(found->second.gpuHandle.ptr)
                };
            }

            void SetWaitSemaphore(NativeHandle semaphore) override
            {
                m_waitSemaphore = semaphore;
            }

            void SetSignalSemaphore(NativeHandle semaphore) override
            {
                m_signalSemaphore = semaphore;
            }

            NativeHandle GetUISignalSemaphore() override
            {
                if (!m_initialized)
                    return {};

                if (m_uiFence == nullptr)
                {
                    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_uiFence));
                    if (FAILED(hr))
                    {
                        NLS_LOG_ERROR("DX12UIBridge: Failed to create UI fence, HRESULT=" + std::to_string(hr));
                        return {};
                    }
                    m_uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    if (m_uiFenceEvent == nullptr)
                    {
                        NLS_LOG_ERROR("DX12UIBridge: Failed to create UI fence event");
                        m_uiFence.Reset();
                        return {};
                    }
                }
                return { NLS::Render::RHI::BackendType::DX12, reinterpret_cast<void*>(m_uiFence.Get()) };
            }

            uint64_t GetUISignalValue() const override
            {
                return m_lastSubmittedUiSignalValue;
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

            struct RetainedTextureHandleUse
            {
                uintptr_t textureViewKey = 0u;
                UINT descriptorIndex = 0u;

                bool operator==(const RetainedTextureHandleUse& other) const
                {
                    return textureViewKey == other.textureViewKey &&
                        descriptorIndex == other.descriptorIndex;
                }
            };

            struct RetiredTextureHandleBatch
            {
                UINT64 fenceValue = 0;
                std::vector<RetainedTextureHandleUse> textureHandleUses;
                std::vector<std::shared_ptr<RHITextureView>> textureViews;
            };

            struct QuarantinedUiFrameSubmission
            {
                Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
                Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
                Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
                Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
                Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer;
                Microsoft::WRL::ComPtr<ID3D12Fence> frameFence;
                Microsoft::WRL::ComPtr<ID3D12Fence> uiFence;
                std::vector<RetainedTextureHandleUse> textureHandleUses;
                std::vector<std::shared_ptr<RHITextureView>> textureViews;
                HRESULT signalResult = S_OK;
                UINT64 attemptedFenceValue = 0u;
                UINT backBufferIndex = 0u;
                std::string context;
            };

            struct QuarantinedUiBridgeResources
            {
                Microsoft::WRL::ComPtr<ID3D12Device> device;
                Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
                Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
                Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
                Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
                Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
                Microsoft::WRL::ComPtr<ID3D12Fence> frameFence;
                Microsoft::WRL::ComPtr<ID3D12Fence> uiFence;
                std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> commandAllocators;
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> backBuffers;
                std::deque<RetiredTextureHandleBatch> retiredTextureHandleBatches;
                std::vector<RetainedTextureHandleUse> currentFrameTextureHandleUses;
                std::vector<std::shared_ptr<RHITextureView>> currentFrameTextureViews;
                std::vector<QuarantinedUiFrameSubmission> quarantinedUiFrameSubmissions;
                std::string context;
            };

            static std::vector<QuarantinedUiBridgeResources>& QuarantinedUiBridgeResourcesKeepAlive()
            {
                static std::vector<QuarantinedUiBridgeResources> resources;
                return resources;
            }

            void KeepTextureViewForCurrentFrame(
                const uintptr_t viewKey,
                const UINT descriptorIndex,
                const std::shared_ptr<RHITextureView>& textureView)
            {
                if (textureView == nullptr)
                    return;

                if (std::find(
                    m_currentFrameTextureHandleUses.begin(),
                    m_currentFrameTextureHandleUses.end(),
                    RetainedTextureHandleUse{ viewKey, descriptorIndex }) !=
                    m_currentFrameTextureHandleUses.end())
                {
                    return;
                }

                m_currentFrameTextureHandleUses.push_back({ viewKey, descriptorIndex });
                m_currentFrameTextureViews.push_back(textureView);
            }

            void DiscardCurrentFrameTextureHandles()
            {
                const auto retiredCurrentFrameUses =
                    m_currentFrameRetirementTracker.DiscardCurrentFrame();
                m_currentFrameTextureHandleUses.clear();
                m_currentFrameTextureViews.clear();

                for (const auto& retiredUse : retiredCurrentFrameUses)
                {
                    auto handleIt = m_textureHandles.find(retiredUse.textureViewKey);
                    if (handleIt != m_textureHandles.end() &&
                        handleIt->second.descriptorIndex == retiredUse.descriptorIndex)
                    {
                        m_textureHandles.erase(handleIt);
                    }

                    if (m_textureDescriptorInFlightUseCounts.find(retiredUse.descriptorIndex) ==
                        m_textureDescriptorInFlightUseCounts.end())
                    {
                        ReleaseTextureDescriptorIndex(retiredUse.descriptorIndex);
                    }
                }
            }

            void MarkDriverUnsafeGpuWorkQuarantined(const std::string& reason)
            {
                NLS::Render::Context::MarkLocatedDriverUnsafeGpuWorkQuarantined(reason);
            }

            void MarkDriverDeviceLost(const std::string& reason)
            {
                NLS::Render::Context::MarkLocatedDriverDeviceLost(reason);
            }

            void QuarantineSubmittedUiFrameAfterExecute(
                const UINT backBufferIndex,
                const HRESULT signalResult,
                const UINT64 attemptedFenceValue,
                std::string context)
            {
                QuarantinedUiFrameSubmission submission;
                submission.srvHeap = m_srvHeap;
                submission.rtvHeap = m_rtvHeap;
                if (backBufferIndex < m_commandAllocators.size())
                    submission.commandAllocator = m_commandAllocators[backBufferIndex];
                if (backBufferIndex < m_backBuffers.size())
                    submission.backBuffer = m_backBuffers[backBufferIndex];
                submission.commandList = m_commandList;
                submission.frameFence = m_fence;
                submission.uiFence = m_uiFence;
                submission.textureHandleUses = std::move(m_currentFrameTextureHandleUses);
                submission.textureViews = std::move(m_currentFrameTextureViews);
                submission.signalResult = signalResult;
                submission.attemptedFenceValue = attemptedFenceValue;
                submission.backBufferIndex = backBufferIndex;
                submission.context = std::move(context);
                m_currentFrameRetirementTracker.RetainCurrentFrame();
                m_currentFrameTextureHandleUses.clear();
                m_currentFrameTextureViews.clear();
                m_quarantinedUiFrameSubmissions.push_back(std::move(submission));
                m_swapchainResourcesQuarantined = true;
                MarkDriverUnsafeGpuWorkQuarantined(
                    context +
                    " failed after ExecuteCommandLists; UI resources were quarantined hr=" +
                    std::to_string(signalResult));
            }

            void QuarantineAllUiBridgeResources(std::string context)
            {
                const std::string quarantineContext = context;
                QuarantinedUiBridgeResources resources;
                resources.device = m_device;
                resources.queue = m_queue;
                resources.swapchain = m_swapchain;
                resources.srvHeap = m_srvHeap;
                resources.rtvHeap = m_rtvHeap;
                resources.commandList = m_commandList;
                resources.frameFence = m_fence;
                resources.uiFence = m_uiFence;
                resources.commandAllocators = m_commandAllocators;
                resources.backBuffers = m_backBuffers;
                resources.retiredTextureHandleBatches = m_retiredTextureHandleBatches;
                resources.currentFrameTextureHandleUses = m_currentFrameTextureHandleUses;
                resources.currentFrameTextureViews = m_currentFrameTextureViews;
                resources.quarantinedUiFrameSubmissions = m_quarantinedUiFrameSubmissions;
                resources.context = std::move(context);
                m_currentFrameRetirementTracker.RetainCurrentFrame();
                QuarantinedUiBridgeResourcesKeepAlive().push_back(std::move(resources));
                m_swapchainResourcesQuarantined = true;
                MarkDriverUnsafeGpuWorkQuarantined(
                    "DX12UIBridge quarantined resources: " +
                    quarantineContext);
            }

            bool IsTextureDescriptorReferencedByCurrentFrame(const UINT descriptorIndex) const
            {
                return std::find_if(
                    m_currentFrameTextureHandleUses.begin(),
                    m_currentFrameTextureHandleUses.end(),
                    [descriptorIndex](const RetainedTextureHandleUse& textureHandleUse)
                    {
                        return textureHandleUse.descriptorIndex == descriptorIndex;
                    }) != m_currentFrameTextureHandleUses.end();
            }

            bool IsTextureViewHandleReferencedByCurrentFrame(
                const uintptr_t viewKey,
                const UINT descriptorIndex) const
            {
                return std::find(
                    m_currentFrameTextureHandleUses.begin(),
                    m_currentFrameTextureHandleUses.end(),
                    RetainedTextureHandleUse{ viewKey, descriptorIndex }) !=
                    m_currentFrameTextureHandleUses.end();
            }

            void DiscardCurrentFrameTextureViewHandle(const uintptr_t viewKey)
            {
                m_currentFrameRetirementTracker.RemoveViewKey(viewKey);
                for (size_t index = 0u; index < m_currentFrameTextureHandleUses.size();)
                {
                    if (m_currentFrameTextureHandleUses[index].textureViewKey != viewKey)
                    {
                        ++index;
                        continue;
                    }

                    m_currentFrameTextureHandleUses.erase(m_currentFrameTextureHandleUses.begin() + index);
                    if (index < m_currentFrameTextureViews.size())
                        m_currentFrameTextureViews.erase(m_currentFrameTextureViews.begin() + index);
                }
            }

            void RemoveRetiredTextureViewHandleBatches(const uintptr_t viewKey)
            {
                for (auto batchIt = m_retiredTextureHandleBatches.begin();
                    batchIt != m_retiredTextureHandleBatches.end();)
                {
                    for (size_t index = 0u; index < batchIt->textureHandleUses.size();)
                    {
                        if (batchIt->textureHandleUses[index].textureViewKey != viewKey)
                        {
                            ++index;
                            continue;
                        }

                        const UINT descriptorIndex = batchIt->textureHandleUses[index].descriptorIndex;
                        auto useCountIt = m_textureDescriptorInFlightUseCounts.find(descriptorIndex);
                        if (useCountIt != m_textureDescriptorInFlightUseCounts.end())
                        {
                            if (useCountIt->second > 1u)
                                --useCountIt->second;
                            else
                                m_textureDescriptorInFlightUseCounts.erase(useCountIt);
                        }

                        ReleaseTextureDescriptorIndex(descriptorIndex);

                        batchIt->textureHandleUses.erase(batchIt->textureHandleUses.begin() + index);
                        if (index < batchIt->textureViews.size())
                            batchIt->textureViews.erase(batchIt->textureViews.begin() + index);
                    }

                    if (batchIt->textureHandleUses.empty())
                        batchIt = m_retiredTextureHandleBatches.erase(batchIt);
                    else
                        ++batchIt;
                }
            }

            void RetainCurrentFrameTextureHandles(const UINT64 fenceValue)
            {
                m_currentFrameRetirementTracker.RetainCurrentFrame();
                if (m_currentFrameTextureViews.empty())
                    return;

                for (const auto& textureHandleUse : m_currentFrameTextureHandleUses)
                    ++m_textureDescriptorInFlightUseCounts[textureHandleUse.descriptorIndex];

                m_retiredTextureHandleBatches.push_back({
                    fenceValue,
                    std::move(m_currentFrameTextureHandleUses),
                    std::move(m_currentFrameTextureViews)
                });

                m_currentFrameTextureHandleUses.clear();
                m_currentFrameTextureViews.clear();
            }

            void RetireCompletedTextureHandles()
            {
                if (m_fence == nullptr)
                    return;

                const UINT64 completedFenceValue = m_fence->GetCompletedValue();
                while (!m_retiredTextureHandleBatches.empty() &&
                    m_retiredTextureHandleBatches.front().fenceValue <= completedFenceValue)
                {
                    auto batch = std::move(m_retiredTextureHandleBatches.front());
                    m_retiredTextureHandleBatches.pop_front();

                    for (const auto& textureHandleUse : batch.textureHandleUses)
                    {
                        auto useCountIt = m_textureDescriptorInFlightUseCounts.find(textureHandleUse.descriptorIndex);
                        if (useCountIt != m_textureDescriptorInFlightUseCounts.end())
                        {
                            if (useCountIt->second > 1u)
                            {
                                --useCountIt->second;
                                continue;
                            }

                            m_textureDescriptorInFlightUseCounts.erase(useCountIt);
                        }

                        auto handleIt = m_textureHandles.find(textureHandleUse.textureViewKey);
                        const bool descriptorReferencedByCurrentFrame =
                            IsTextureDescriptorReferencedByCurrentFrame(textureHandleUse.descriptorIndex);
                        const bool sameViewReferencedByCurrentFrame =
                            IsTextureViewHandleReferencedByCurrentFrame(
                                textureHandleUse.textureViewKey,
                                textureHandleUse.descriptorIndex);
                        if (handleIt != m_textureHandles.end() &&
                            handleIt->second.descriptorIndex == textureHandleUse.descriptorIndex)
                        {
                            if (!descriptorReferencedByCurrentFrame || !sameViewReferencedByCurrentFrame)
                                m_textureHandles.erase(handleIt);
                        }

                        ReleaseTextureDescriptorIndex(textureHandleUse.descriptorIndex);
                    }
                }
            }

            void ReleaseTextureDescriptorIndex(const UINT descriptorIndex)
            {
                if (descriptorIndex == 0u)
                    return;

                if (IsTextureDescriptorReferencedByCurrentFrame(descriptorIndex))
                    return;

                if (std::find(
                    m_freeTextureDescriptorIndices.begin(),
                    m_freeTextureDescriptorIndices.end(),
                    descriptorIndex) == m_freeTextureDescriptorIndices.end())
                {
                    m_freeTextureDescriptorIndices.push_back(descriptorIndex);
                }
            }

            bool EnsureSwapchainRenderResources(const NativeRenderDeviceInfo& nativeInfo)
            {
                if (!m_device || !m_queue)
                    return false;
                if (m_swapchainResourcesQuarantined)
                {
                    NLS_LOG_ERROR("DX12UIBridge::EnsureSwapchainRenderResources: UI swapchain resources are quarantined");
                    return false;
                }

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
                if (m_swapchainResourcesQuarantined)
                {
                    NLS_LOG_ERROR(
                        "DX12UIBridge::ReleaseSwapchainRenderResources: preserving quarantined swapchain UI resources");
                    return;
                }
                if (!WaitForSubmittedUiWork())
                {
                    m_swapchainResourcesQuarantined = true;
                    MarkDriverUnsafeGpuWorkQuarantined(
                        "DX12UIBridge::ReleaseSwapchainRenderResources failed to drain submitted UI work");
                    NLS_LOG_ERROR(
                        "DX12UIBridge::ReleaseSwapchainRenderResources: preserving swapchain UI resources because submitted work could not be drained");
                    return;
                }

                m_commandAllocators.clear();
                m_backBuffers.clear();
                m_commandList.Reset();
                m_rtvHeap.Reset();
                m_frameFenceTracker.ResetBackbufferCount(0u);
            }

            bool WaitForSubmittedUiWork()
            {
                if (m_fence && m_queue && m_fenceEvent != nullptr)
                {
                    const UINT64 fenceValue = ++m_fenceValue;
                    const HRESULT signalHr = [&]()
                    {
                        DX12::ScopedDX12QueueLock queueLock(m_queue.Get());
                        return m_queue->Signal(m_fence.Get(), fenceValue);
                    }();
                    if (FAILED(signalHr))
                    {
                        NLS_LOG_ERROR(
                            "DX12UIBridge::WaitForSubmittedUiWork: queue signal failed hr=" +
                            std::to_string(signalHr));
                        return false;
                    }
                    return WaitForDX12UIFence(
                        m_fence.Get(),
                        m_fenceEvent,
                        fenceValue,
                        "DX12UIBridge::WaitForSubmittedUiWork");
                }
                return true;
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

                if (!m_swapchainResourcesQuarantined && m_fence && m_queue)
                {
                    const UINT64 fenceValue = ++m_fenceValue;
                    const HRESULT signalHr = [&]()
                    {
                        DX12::ScopedDX12QueueLock queueLock(m_queue.Get());
                        return m_queue->Signal(m_fence.Get(), fenceValue);
                    }();
                    if (FAILED(signalHr))
                    {
                        QuarantineAllUiBridgeResources("DX12UIBridge::Shutdown queue signal failed");
                        NLS_LOG_ERROR(
                            "DX12UIBridge::Shutdown: queue signal failed hr=" +
                            std::to_string(signalHr));
                    }
                    else
                    {
                        WaitForDX12UIFence(
                            m_fence.Get(),
                            m_fenceEvent,
                            fenceValue,
                            "DX12UIBridge::Shutdown");
                    }
                }

                ReleaseSwapchainRenderResources();
                if (m_swapchainResourcesQuarantined)
                {
                    QuarantineAllUiBridgeResources("DX12UIBridge::Shutdown preserving quarantined resources");
                    m_initialized = false;
                    return;
                }
                DiscardCurrentFrameTextureHandles();
                m_retiredTextureHandleBatches.clear();
                m_textureDescriptorInFlightUseCounts.clear();
                m_textureHandles.clear();
                m_freeTextureDescriptorIndices.clear();
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
            std::unordered_map<UINT, uint32_t> m_textureDescriptorInFlightUseCounts;
            std::deque<RetiredTextureHandleBatch> m_retiredTextureHandleBatches;
            std::vector<QuarantinedUiFrameSubmission> m_quarantinedUiFrameSubmissions;
            std::vector<RetainedTextureHandleUse> m_currentFrameTextureHandleUses;
            std::vector<std::shared_ptr<RHITextureView>> m_currentFrameTextureViews;
            RHIUICurrentFrameTextureRetirementTracker m_currentFrameRetirementTracker;
            std::vector<UINT> m_freeTextureDescriptorIndices;
            HANDLE m_fenceEvent = nullptr;
            UINT64 m_fenceValue = 0;
            UINT m_rtvDescriptorSize = 0;
            UINT m_srvDescriptorSize = 0;
            UINT m_srvDescriptorCapacity = 0;
            UINT m_nextTextureDescriptorIndex = 1;
            uint64_t m_lastSubmittedUiSignalValue = 0u;
            DXGI_FORMAT m_backbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            uint32_t m_backbufferWidth = 0;
            uint32_t m_backbufferHeight = 0;
            uint32_t m_imageCount = 0;
            bool m_initialized = false;
            bool m_swapchainResourcesQuarantined = false;
            NativeHandle m_waitSemaphore;
            NativeHandle m_signalSemaphore;
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
