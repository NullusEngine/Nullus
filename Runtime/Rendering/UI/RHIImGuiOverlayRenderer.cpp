#include "Rendering/UI/RHIImGuiOverlayRenderer.h"

#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/Resources/Shader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

namespace NLS::Render::UI
{
    namespace
    {
        constexpr const char* kOverlayShaderPath = ":Shaders/RHIImGuiOverlay.hlsl";

        struct OverlayProjectionConstants
        {
            float scale[2] {};
            float translate[2] {};
        };

        bool IsSnapshotRecordable(const UiDrawDataSnapshot& snapshot)
        {
            return snapshot.hasVisibleDraws &&
                snapshot.displaySize[0] > 0.0f &&
                snapshot.displaySize[1] > 0.0f &&
                snapshot.framebufferScale[0] > 0.0f &&
                snapshot.framebufferScale[1] > 0.0f;
        }

        std::optional<RHI::RHIRect2D> BuildScissor(
            const UiDrawDataSnapshot& snapshot,
            const UiDrawCommandSnapshot& command)
        {
            const float clipMinX = (command.clipRect[0] - snapshot.displayPos[0]) * snapshot.framebufferScale[0];
            const float clipMinY = (command.clipRect[1] - snapshot.displayPos[1]) * snapshot.framebufferScale[1];
            const float clipMaxX = (command.clipRect[2] - snapshot.displayPos[0]) * snapshot.framebufferScale[0];
            const float clipMaxY = (command.clipRect[3] - snapshot.displayPos[1]) * snapshot.framebufferScale[1];

            const float framebufferWidth = snapshot.displaySize[0] * snapshot.framebufferScale[0];
            const float framebufferHeight = snapshot.displaySize[1] * snapshot.framebufferScale[1];
            const int32_t minX = static_cast<int32_t>(std::floor(std::clamp(clipMinX, 0.0f, framebufferWidth)));
            const int32_t minY = static_cast<int32_t>(std::floor(std::clamp(clipMinY, 0.0f, framebufferHeight)));
            const int32_t maxX = static_cast<int32_t>(std::ceil(std::clamp(clipMaxX, 0.0f, framebufferWidth)));
            const int32_t maxY = static_cast<int32_t>(std::ceil(std::clamp(clipMaxY, 0.0f, framebufferHeight)));
            if (maxX <= minX || maxY <= minY)
                return std::nullopt;

            RHI::RHIRect2D rect;
            rect.x = minX;
            rect.y = minY;
            rect.width = static_cast<uint32_t>(maxX - minX);
            rect.height = static_cast<uint32_t>(maxY - minY);
            return rect;
        }

        std::shared_ptr<RHI::RHIDevice> MakeNonOwningDeviceHandle(RHI::RHIDevice& device)
        {
            return std::shared_ptr<RHI::RHIDevice>(&device, [](RHI::RHIDevice*) {});
        }

        OverlayProjectionConstants BuildProjectionConstants(const UiDrawDataSnapshot& snapshot)
        {
            OverlayProjectionConstants constants;
            constants.scale[0] = 2.0f / snapshot.displaySize[0];
            constants.scale[1] = -2.0f / snapshot.displaySize[1];
            constants.translate[0] = -1.0f - snapshot.displayPos[0] * constants.scale[0];
            constants.translate[1] = 1.0f - snapshot.displayPos[1] * constants.scale[1];
            return constants;
        }

        Resources::Shader* ResolveOverlayShader()
        {
            if (!Core::ServiceLocator::Contains<Core::ResourceManagement::ShaderManager>())
                return nullptr;

            return Core::ServiceLocator::Get<Core::ResourceManagement::ShaderManager>().GetResource(
                kOverlayShaderPath,
                true);
        }

        void ResetOverlayPipelineCache(
            std::shared_ptr<RHI::RHIBindingLayout>& fontAtlasBindingLayout,
            std::shared_ptr<RHI::RHIPipelineLayout>& pipelineLayout,
            std::shared_ptr<RHI::RHIShaderModule>& vertexShaderModule,
            std::shared_ptr<RHI::RHIShaderModule>& fragmentShaderModule,
            std::shared_ptr<RHI::RHIGraphicsPipeline>& graphicsPipeline,
            uint64_t& pipelineDeviceCacheIdentity)
        {
            fontAtlasBindingLayout.reset();
            pipelineLayout.reset();
            vertexShaderModule.reset();
            fragmentShaderModule.reset();
            graphicsPipeline.reset();
            pipelineDeviceCacheIdentity = 0u;
        }
    }

    RHIImGuiOverlayRenderer::RHIImGuiOverlayRenderer(RHIImGuiTextureRegistry* textureRegistry)
        : m_textureRegistry(textureRegistry)
    {
    }

    RHIImGuiOverlayRecordingResult RHIImGuiOverlayRenderer::PrepareFrameResources(
        RHI::RHIDevice& device,
        RHI::RHICommandBuffer& commandBuffer,
        const UiDrawDataSnapshot& snapshot,
        const size_t frameResourceSlot)
    {
        std::lock_guard lock(m_mutex);
        RHIImGuiOverlayRecordingResult result;
        if (!IsSnapshotRecordable(snapshot))
        {
            result.success = true;
            result.message = "UI snapshot has no recordable visible draws";
            return result;
        }

        const auto telemetryBefore = m_dynamicBufferTelemetry;
        const auto copyStart = std::chrono::steady_clock::now();
        const size_t vertexBytes = static_cast<size_t>(snapshot.totalVertexCount) * sizeof(UiDrawVertex);
        const size_t indexBytes = static_cast<size_t>(snapshot.totalIndexCount) * sizeof(uint32_t);
        const bool fontAtlasWasUploaded = m_fontAtlas.IsUploaded();
        if (!EnsureGraphicsPipeline(device, result.message) ||
            !m_fontAtlas.EnsureUploaded(device, commandBuffer, m_fontAtlasBindingLayout, result.message) ||
            !EnsureRegisteredTextureBindingSets(device, snapshot, result.message) ||
            !EnsureDynamicBuffers(frameResourceSlot, device, vertexBytes, indexBytes, result.message) ||
            !UploadDynamicBuffers(frameResourceSlot, snapshot, result.message))
        {
            result.success = false;
            return result;
        }

        auto& buffers = m_dynamicBuffersByFrameSlot[frameResourceSlot];
        buffers.preparedFrameId = snapshot.frameId;
        buffers.preparedVertexCount = snapshot.totalVertexCount;
        buffers.preparedIndexCount = snapshot.totalIndexCount;
        buffers.preparedSnapshotAddress = &snapshot;
        buffers.hasPreparedSnapshot = true;
        buffers.fontAtlasUploadTransitionRequired = !fontAtlasWasUploaded && m_fontAtlas.IsUploaded();
        buffers.preparedFontAtlasTextureView = m_fontAtlas.TextureView();
        buffers.preparedFontAtlasBindingSet = m_fontAtlas.BindingSet();
        m_dynamicBufferTelemetry.totalCpuCopyTimeNanoseconds += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - copyStart).count());
        result.dynamicBufferTelemetry.allocationCount =
            m_dynamicBufferTelemetry.allocationCount - telemetryBefore.allocationCount;
        result.dynamicBufferTelemetry.reallocationCount =
            m_dynamicBufferTelemetry.reallocationCount - telemetryBefore.reallocationCount;
        result.dynamicBufferTelemetry.totalCpuCopyTimeNanoseconds =
            m_dynamicBufferTelemetry.totalCpuCopyTimeNanoseconds - telemetryBefore.totalCpuCopyTimeNanoseconds;

        RecordDynamicBufferUploadVisibilityBarrier(frameResourceSlot, commandBuffer);
        result.success = true;
        result.message = "prepared UI overlay dynamic buffers";
        return result;
    }

    RHIImGuiOverlayRecordingResult RHIImGuiOverlayRenderer::RecordPrepared(
        RHI::RHICommandBuffer& commandBuffer,
        const UiDrawDataSnapshot& snapshot,
        const size_t frameResourceSlot)
    {
        std::lock_guard lock(m_mutex);
        return RecordInternal(commandBuffer, snapshot, frameResourceSlot, true);
    }

    RHIImGuiOverlayPreparedResourceSnapshot RHIImGuiOverlayRenderer::GetPreparedResourceSnapshot(
        const UiDrawDataSnapshot& snapshot,
        const size_t frameResourceSlot) const
    {
        std::lock_guard lock(m_mutex);
        RHIImGuiOverlayPreparedResourceSnapshot resources;
        const auto buffersIt = m_dynamicBuffersByFrameSlot.find(frameResourceSlot);
        if (buffersIt == m_dynamicBuffersByFrameSlot.end())
            return resources;

        const auto& buffers = buffersIt->second;
        if (!buffers.hasPreparedSnapshot ||
            buffers.preparedSnapshotAddress != &snapshot ||
            buffers.preparedFrameId != snapshot.frameId ||
            buffers.preparedVertexCount != snapshot.totalVertexCount ||
            buffers.preparedIndexCount != snapshot.totalIndexCount)
        {
            return resources;
        }

        resources.vertexBuffer = buffers.vertexBuffer;
        resources.indexBuffer = buffers.indexBuffer;
        if (buffers.preparedFontAtlasTextureView != nullptr)
        {
            resources.fontAtlasTextureView = buffers.preparedFontAtlasTextureView;
            resources.fontAtlasUploadTransitionRequired = buffers.fontAtlasUploadTransitionRequired;
        }

        if (m_textureRegistry != nullptr)
        {
            for (const auto& drawList : snapshot.drawLists)
            {
                for (const auto& command : drawList.commands)
                {
                    if (command.elementCount == 0u ||
                        command.callbackKind == UiDrawCallbackKind::Unsupported ||
                        command.hasUnsupportedTextureId ||
                        command.textureId.IsFontAtlas())
                    {
                        continue;
                    }

                    const auto entry = m_textureRegistry->ResolveForFrame(command.textureId, snapshot.frameId);
                    if (!entry.has_value() || entry->textureView == nullptr)
                        continue;

                    if (std::find(
                        resources.registeredTextureViews.begin(),
                        resources.registeredTextureViews.end(),
                        entry->textureView) == resources.registeredTextureViews.end())
                    {
                        resources.registeredTextureViews.push_back(entry->textureView);
                    }
                }
            }
        }

        return resources;
    }

    void RHIImGuiOverlayRenderer::SetTextureRegistry(RHIImGuiTextureRegistry* textureRegistry)
    {
        std::lock_guard lock(m_mutex);
        m_textureRegistry = textureRegistry;
    }

    void RHIImGuiOverlayRenderer::InvalidateFontAtlas(const uint64_t retireFrameId)
    {
        std::lock_guard lock(m_mutex);
        m_fontAtlas.Invalidate(retireFrameId);
    }

    void RHIImGuiOverlayRenderer::ReleaseRetiredResourcesUpTo(const uint64_t completedFrameId)
    {
        std::lock_guard lock(m_mutex);
        m_fontAtlas.ReleaseRetiredResourcesUpTo(completedFrameId);
    }

    bool RHIImGuiOverlayRenderer::EnsureDynamicBuffers(
        const size_t frameResourceSlot,
        RHI::RHIDevice& device,
        const size_t vertexBytes,
        const size_t indexBytes,
        std::string& errorMessage)
    {
        if (vertexBytes == 0u || indexBytes == 0u)
        {
            errorMessage = "UI overlay dynamic buffers require non-empty vertex and index data";
            return false;
        }

        auto& buffers = m_dynamicBuffersByFrameSlot[frameResourceSlot];
        const uint64_t deviceCacheIdentity = device.GetCacheIdentity();
        if (buffers.deviceCacheIdentity != 0u && buffers.deviceCacheIdentity != deviceCacheIdentity)
        {
            buffers.vertexBuffer.reset();
            buffers.indexBuffer.reset();
            buffers.vertexBufferCapacityBytes = 0u;
            buffers.indexBufferCapacityBytes = 0u;
            buffers.preparedFrameId = 0u;
            buffers.preparedVertexCount = 0u;
            buffers.preparedIndexCount = 0u;
            buffers.preparedSnapshotAddress = nullptr;
            buffers.hasPreparedSnapshot = false;
            buffers.preparedFontAtlasTextureView.reset();
            buffers.preparedFontAtlasBindingSet.reset();
        }
        buffers.deviceCacheIdentity = deviceCacheIdentity;

        if (buffers.vertexBuffer == nullptr || buffers.vertexBufferCapacityBytes < vertexBytes)
        {
            if (buffers.vertexBuffer != nullptr)
                ++m_dynamicBufferTelemetry.reallocationCount;
            else
                ++m_dynamicBufferTelemetry.allocationCount;
            RHI::RHIBufferDesc desc;
            desc.size = vertexBytes;
            desc.usage = RHI::BufferUsageFlags::Vertex;
            desc.memoryUsage = RHI::MemoryUsage::CPUToGPU;
            desc.debugName = "RHIImGuiOverlayVertexBuffer";
            buffers.vertexBuffer = device.CreateBuffer(desc);
            if (buffers.vertexBuffer == nullptr)
            {
                errorMessage = "failed to create UI overlay dynamic vertex buffer";
                buffers.vertexBufferCapacityBytes = 0u;
                return false;
            }
            buffers.vertexBufferCapacityBytes = desc.size;
        }

        if (buffers.indexBuffer == nullptr || buffers.indexBufferCapacityBytes < indexBytes)
        {
            if (buffers.indexBuffer != nullptr)
                ++m_dynamicBufferTelemetry.reallocationCount;
            else
                ++m_dynamicBufferTelemetry.allocationCount;
            RHI::RHIBufferDesc desc;
            desc.size = indexBytes;
            desc.usage = RHI::BufferUsageFlags::Index;
            desc.memoryUsage = RHI::MemoryUsage::CPUToGPU;
            desc.debugName = "RHIImGuiOverlayIndexBuffer";
            buffers.indexBuffer = device.CreateBuffer(desc);
            if (buffers.indexBuffer == nullptr)
            {
                errorMessage = "failed to create UI overlay dynamic index buffer";
                buffers.indexBufferCapacityBytes = 0u;
                return false;
            }
            buffers.indexBufferCapacityBytes = desc.size;
        }

        return true;
    }

    RHIImGuiOverlayDynamicBufferTelemetry RHIImGuiOverlayRenderer::GetDynamicBufferTelemetry() const
    {
        std::lock_guard lock(m_mutex);
        return m_dynamicBufferTelemetry;
    }

    bool RHIImGuiOverlayRenderer::EnsureRegisteredTextureBindingSets(
        RHI::RHIDevice& device,
        const UiDrawDataSnapshot& snapshot,
        std::string& errorMessage)
    {
        if (m_textureRegistry == nullptr)
            return true;

        for (const auto& drawList : snapshot.drawLists)
        {
            for (const auto& command : drawList.commands)
            {
                if (command.elementCount == 0u ||
                    command.callbackKind == UiDrawCallbackKind::Unsupported ||
                    command.hasUnsupportedTextureId ||
                    command.textureId.IsFontAtlas())
                {
                    continue;
                }

                if (!m_textureRegistry->EnsureBindingSet(
                    device,
                    m_fontAtlasBindingLayout,
                    m_fontAtlas.Sampler(),
                    command.textureId,
                    errorMessage))
                {
                    return false;
                }
            }
        }

        return true;
    }

    bool RHIImGuiOverlayRenderer::EnsureGraphicsPipeline(
        RHI::RHIDevice& device,
        std::string& errorMessage)
    {
        const uint64_t deviceCacheIdentity = device.GetCacheIdentity();
        if (m_pipelineDeviceCacheIdentity != 0u && m_pipelineDeviceCacheIdentity != deviceCacheIdentity)
        {
            ResetOverlayPipelineCache(
                m_fontAtlasBindingLayout,
                m_pipelineLayout,
                m_vertexShaderModule,
                m_fragmentShaderModule,
                m_graphicsPipeline,
                m_pipelineDeviceCacheIdentity);
        }

        if (m_graphicsPipeline != nullptr)
            return true;

        m_pipelineDeviceCacheIdentity = deviceCacheIdentity;
        if (m_pipelineLayout == nullptr)
        {
            if (m_fontAtlasBindingLayout == nullptr)
            {
                RHI::RHIBindingLayoutDesc bindingLayoutDesc;
                bindingLayoutDesc.debugName = "RHIImGuiFontAtlasBindingLayout";
                bindingLayoutDesc.entries.push_back({
                    "FontAtlasTexture",
                    RHI::BindingType::Texture,
                    0u,
                    0u,
                    1u,
                    RHI::ShaderStageMask::Fragment,
                    0u,
                    0u
                });
                bindingLayoutDesc.entries.push_back({
                    "FontAtlasSampler",
                    RHI::BindingType::Sampler,
                    0u,
                    1u,
                    1u,
                    RHI::ShaderStageMask::Fragment,
                    0u,
                    0u
                });
                m_fontAtlasBindingLayout = device.CreateBindingLayout(bindingLayoutDesc);
                if (m_fontAtlasBindingLayout == nullptr)
                {
                    errorMessage = "failed to create UI font atlas binding layout";
                    return false;
                }
            }
            RHI::RHIPipelineLayoutDesc layoutDesc;
            layoutDesc.debugName = "RHIImGuiOverlayPipelineLayout";
            layoutDesc.bindingLayouts.push_back(m_fontAtlasBindingLayout);
            layoutDesc.pushConstants.push_back({
                RHI::ShaderStageMask::Vertex,
                0u,
                static_cast<uint32_t>(sizeof(OverlayProjectionConstants)),
                0u,
                0u
            });
            m_pipelineLayout = device.CreatePipelineLayout(layoutDesc);
            if (m_pipelineLayout == nullptr)
            {
                errorMessage = "failed to create UI overlay pipeline layout";
                return false;
            }
        }

        auto* overlayShader = ResolveOverlayShader();
        if (overlayShader == nullptr)
        {
            errorMessage = "failed to load UI overlay shader artifact";
            return false;
        }

        const auto deviceHandle = MakeNonOwningDeviceHandle(device);
        if (m_vertexShaderModule == nullptr)
        {
            m_vertexShaderModule = overlayShader->GetOrCreateExplicitShaderModule(
                deviceHandle,
                ShaderCompiler::ShaderStage::Vertex);
            if (m_vertexShaderModule == nullptr)
            {
                errorMessage = "failed to create UI overlay vertex shader module";
                return false;
            }
        }

        if (m_fragmentShaderModule == nullptr)
        {
            m_fragmentShaderModule = overlayShader->GetOrCreateExplicitShaderModule(
                deviceHandle,
                ShaderCompiler::ShaderStage::Pixel);
            if (m_fragmentShaderModule == nullptr)
            {
                errorMessage = "failed to create UI overlay fragment shader module";
                return false;
            }
        }

        RHI::RHIGraphicsPipelineDesc desc;
        desc.debugName = "RHIImGuiOverlayPipeline";
        desc.pipelineLayout = m_pipelineLayout;
        desc.vertexShader = m_vertexShaderModule;
        desc.fragmentShader = m_fragmentShaderModule;
        desc.rasterState.cullEnabled = false;
        desc.depthStencilState.depthTest = false;
        desc.depthStencilState.depthWrite = false;
        desc.blendState.enabled = true;
        desc.blendState.renderTargets.push_back({
            true,
            RHI::RHIBlendFactor::SrcAlpha,
            RHI::RHIBlendFactor::InvSrcAlpha,
            RHI::RHIBlendOp::Add,
            RHI::RHIBlendFactor::One,
            RHI::RHIBlendFactor::InvSrcAlpha,
            RHI::RHIBlendOp::Add,
            RHI::RHIColorWriteMask::All
        });
        desc.vertexBuffers.push_back({ 0u, static_cast<uint32_t>(sizeof(UiDrawVertex)), false });
        desc.vertexAttributes.push_back({ 0u, 0u, 0u, sizeof(float) * 2u });
        desc.vertexAttributes.push_back({ 1u, 0u, sizeof(float) * 2u, sizeof(float) * 2u });
        desc.vertexAttributes.push_back({ 3u, 0u, sizeof(float) * 4u, sizeof(float) * 4u });

        m_graphicsPipeline = device.CreateGraphicsPipeline(desc);
        if (m_graphicsPipeline == nullptr)
        {
            errorMessage = "failed to create UI overlay graphics pipeline";
            return false;
        }

        return true;
    }

    bool RHIImGuiOverlayRenderer::UploadDynamicBuffers(
        const size_t frameResourceSlot,
        const UiDrawDataSnapshot& snapshot,
        std::string& errorMessage)
    {
        const auto buffersIt = m_dynamicBuffersByFrameSlot.find(frameResourceSlot);
        if (buffersIt == m_dynamicBuffersByFrameSlot.end() ||
            buffersIt->second.vertexBuffer == nullptr ||
            buffersIt->second.indexBuffer == nullptr)
        {
            errorMessage = "UI overlay dynamic buffers are not allocated";
            return false;
        }
        const auto& buffers = buffersIt->second;

        m_cpuVertices.clear();
        m_cpuIndices.clear();
        m_cpuVertices.reserve(snapshot.totalVertexCount);
        m_cpuIndices.reserve(snapshot.totalIndexCount);
        for (const auto& drawList : snapshot.drawLists)
        {
            m_cpuVertices.insert(m_cpuVertices.end(), drawList.vertices.begin(), drawList.vertices.end());
            m_cpuIndices.insert(m_cpuIndices.end(), drawList.indices.begin(), drawList.indices.end());
        }

        if (m_cpuVertices.empty() || m_cpuIndices.empty())
        {
            errorMessage = "UI overlay snapshot did not contain copied vertex/index data";
            return false;
        }

        RHI::RHIBufferUploadDesc vertexUpload;
        vertexUpload.data = m_cpuVertices.data();
        vertexUpload.dataSize = m_cpuVertices.size() * sizeof(UiDrawVertex);
        vertexUpload.destinationOffset = 0u;
        vertexUpload.debugName = "RHIImGuiOverlayVertexUpload";
        const auto vertexResult = buffers.vertexBuffer->UpdateData(vertexUpload);
        if (!vertexResult.Succeeded())
        {
            errorMessage = vertexResult.message.empty()
                ? "failed to upload UI overlay dynamic vertex buffer"
                : vertexResult.message;
            return false;
        }

        RHI::RHIBufferUploadDesc indexUpload;
        indexUpload.data = m_cpuIndices.data();
        indexUpload.dataSize = m_cpuIndices.size() * sizeof(uint32_t);
        indexUpload.destinationOffset = 0u;
        indexUpload.debugName = "RHIImGuiOverlayIndexUpload";
        const auto indexResult = buffers.indexBuffer->UpdateData(indexUpload);
        if (!indexResult.Succeeded())
        {
            errorMessage = indexResult.message.empty()
                ? "failed to upload UI overlay dynamic index buffer"
                : indexResult.message;
            return false;
        }

        return true;
    }

    bool RHIImGuiOverlayRenderer::BindPreparedDynamicBuffers(
        const size_t frameResourceSlot,
        const UiDrawDataSnapshot& snapshot,
        RHI::RHICommandBuffer& commandBuffer,
        std::string& errorMessage)
    {
        const auto buffersIt = m_dynamicBuffersByFrameSlot.find(frameResourceSlot);
        if (buffersIt == m_dynamicBuffersByFrameSlot.end() ||
            buffersIt->second.vertexBuffer == nullptr ||
            buffersIt->second.indexBuffer == nullptr)
        {
            errorMessage = "UI overlay dynamic buffers are not prepared for this frame resource slot";
            return false;
        }

        const auto& buffers = buffersIt->second;
        if (!buffers.hasPreparedSnapshot ||
            buffers.preparedSnapshotAddress != &snapshot ||
            buffers.preparedFrameId != snapshot.frameId ||
            buffers.preparedVertexCount != snapshot.totalVertexCount ||
            buffers.preparedIndexCount != snapshot.totalIndexCount)
        {
            errorMessage = "prepared UI overlay dynamic buffers do not match the snapshot being recorded";
            return false;
        }

        commandBuffer.BindVertexBuffer(0u, { buffers.vertexBuffer, 0u, static_cast<uint32_t>(sizeof(UiDrawVertex)) });
        commandBuffer.BindIndexBuffer({ buffers.indexBuffer, 0u, RHI::IndexType::UInt32 });
        return true;
    }

    void RHIImGuiOverlayRenderer::RecordDynamicBufferUploadVisibilityBarrier(
        const size_t frameResourceSlot,
        RHI::RHICommandBuffer& commandBuffer)
    {
        const auto buffersIt = m_dynamicBuffersByFrameSlot.find(frameResourceSlot);
        if (buffersIt == m_dynamicBuffersByFrameSlot.end() ||
            buffersIt->second.vertexBuffer == nullptr ||
            buffersIt->second.indexBuffer == nullptr)
        {
            return;
        }

        const auto& buffers = buffersIt->second;
        RHI::RHIBarrierDesc uploadVisibility;
        uploadVisibility.bufferBarriers.reserve(2u);
        uploadVisibility.bufferBarriers.push_back({
            buffers.vertexBuffer,
            RHI::ResourceState::GenericRead,
            RHI::ResourceState::GenericRead,
            RHI::PipelineStageMask::Host,
            RHI::PipelineStageMask::VertexInput,
            RHI::AccessMask::HostWrite,
            RHI::AccessMask::VertexRead
        });
        uploadVisibility.bufferBarriers.push_back({
            buffers.indexBuffer,
            RHI::ResourceState::GenericRead,
            RHI::ResourceState::GenericRead,
            RHI::PipelineStageMask::Host,
            RHI::PipelineStageMask::VertexInput,
            RHI::AccessMask::HostWrite,
            RHI::AccessMask::IndexRead
        });
        commandBuffer.Barrier(uploadVisibility);
    }

    RHIImGuiOverlayRecordingResult RHIImGuiOverlayRenderer::RecordInternal(
        RHI::RHICommandBuffer& commandBuffer,
        const UiDrawDataSnapshot& snapshot,
        const size_t frameResourceSlot,
        const bool bindPreparedDynamicBuffers)
    {
        RHIImGuiOverlayRecordingResult result;
        if (!IsSnapshotRecordable(snapshot))
        {
            result.success = true;
            result.message = "UI snapshot has no recordable visible draws";
            return result;
        }

        if (bindPreparedDynamicBuffers)
        {
            if (!BindPreparedDynamicBuffers(frameResourceSlot, snapshot, commandBuffer, result.message))
            {
                result.success = false;
                return result;
            }
            if (m_graphicsPipeline == nullptr)
            {
                result.success = false;
                result.message = "UI overlay graphics pipeline is not prepared";
                return result;
            }
            commandBuffer.BindGraphicsPipeline(m_graphicsPipeline);
        }

        commandBuffer.SetViewport({
            0.0f,
            0.0f,
            snapshot.displaySize[0] * snapshot.framebufferScale[0],
            snapshot.displaySize[1] * snapshot.framebufferScale[1],
            0.0f,
            1.0f
        });
        const auto projectionConstants = BuildProjectionConstants(snapshot);
        commandBuffer.PushConstants(
            RHI::ShaderStageMask::Vertex,
            0u,
            static_cast<uint32_t>(sizeof(projectionConstants)),
            &projectionConstants);

        uint64_t recordedDrawCount = 0u;
        uint64_t globalVertexOffset = 0u;
        uint64_t globalIndexOffset = 0u;
        std::shared_ptr<RHI::RHIBindingSet> currentBindingSet;
        for (const auto& drawList : snapshot.drawLists)
        {
            for (const auto& command : drawList.commands)
            {
                if (command.elementCount == 0u ||
                    command.callbackKind == UiDrawCallbackKind::Unsupported ||
                    command.hasUnsupportedTextureId)
                {
                    continue;
                }

                const uint64_t indexEnd =
                    static_cast<uint64_t>(command.indexOffset) + static_cast<uint64_t>(command.elementCount);
                if (indexEnd > drawList.indices.size() ||
                    command.vertexOffset > drawList.vertices.size())
                {
                    result.success = false;
                    result.recordedDraws = recordedDrawCount > 0u;
                    result.message = "UI draw command references vertices or indices outside the copied snapshot";
                    return result;
                }
                if (bindPreparedDynamicBuffers &&
                    globalIndexOffset + static_cast<uint64_t>(command.indexOffset) >
                        static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()))
                {
                    result.success = false;
                    result.recordedDraws = recordedDrawCount > 0u;
                    result.message = "UI draw command global index offset exceeds RHI DrawIndexed range";
                    return result;
                }
                if (bindPreparedDynamicBuffers &&
                    globalVertexOffset + static_cast<uint64_t>(command.vertexOffset) >
                        static_cast<uint64_t>((std::numeric_limits<int32_t>::max)()))
                {
                    result.success = false;
                    result.recordedDraws = recordedDrawCount > 0u;
                    result.message = "UI draw command global vertex offset exceeds RHI DrawIndexed range";
                    return result;
                }

                const auto scissor = BuildScissor(snapshot, command);
                if (!scissor.has_value())
                    continue;

                const uint32_t firstIndex = bindPreparedDynamicBuffers
                    ? static_cast<uint32_t>(globalIndexOffset + command.indexOffset)
                    : command.indexOffset;
                const int32_t vertexOffset = bindPreparedDynamicBuffers
                    ? static_cast<int32_t>(globalVertexOffset + command.vertexOffset)
                    : static_cast<int32_t>(command.vertexOffset);

                if (bindPreparedDynamicBuffers)
                {
                    std::shared_ptr<RHI::RHIBindingSet> desiredBindingSet;
                    if (command.textureId.IsFontAtlas())
                    {
                        const auto buffersIt = m_dynamicBuffersByFrameSlot.find(frameResourceSlot);
                        if (buffersIt == m_dynamicBuffersByFrameSlot.end())
                            continue;

                        desiredBindingSet = buffersIt->second.preparedFontAtlasBindingSet;
                    }
                    else if (m_textureRegistry != nullptr)
                    {
                        const auto textureEntry = m_textureRegistry->ResolveForFrame(command.textureId, snapshot.frameId);
                        if (!textureEntry.has_value() || textureEntry->bindingSet == nullptr)
                            continue;

                        desiredBindingSet = textureEntry->bindingSet;
                    }
                    else
                    {
                        continue;
                    }

                    if (desiredBindingSet == nullptr)
                        continue;

                    if (desiredBindingSet != currentBindingSet)
                    {
                        commandBuffer.BindBindingSet(0u, desiredBindingSet);
                        currentBindingSet = std::move(desiredBindingSet);
                    }
                }

                commandBuffer.SetScissor(*scissor);
                const auto drawResult = commandBuffer.DrawIndexedChecked(
                    command.elementCount,
                    1u,
                    firstIndex,
                    vertexOffset,
                    0u);
                if (!drawResult.Succeeded())
                {
                    result.success = false;
                    result.recordedDraws = recordedDrawCount > 0u;
                    result.message = drawResult.message.empty()
                        ? "UI overlay DrawIndexed failed"
                        : drawResult.message;
                    return result;
                }
                ++recordedDrawCount;
            }
            globalVertexOffset += static_cast<uint64_t>(drawList.vertices.size());
            globalIndexOffset += static_cast<uint64_t>(drawList.indices.size());
        }

        result.success = true;
        result.recordedDraws = recordedDrawCount > 0u;
        std::ostringstream message;
        message << "recorded " << recordedDrawCount << " UI draw command(s)";
        result.message = message.str();
        return result;
    }
}
