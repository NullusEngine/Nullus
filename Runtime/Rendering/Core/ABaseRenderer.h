#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "Rendering/Core/IRenderer.h"
#include "Rendering/Data/FrameInfo.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"
#include "Rendering/Resources/IMesh.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/Entities/Drawable.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include <Math/Vector4.h>
#include "RenderDef.h"

namespace NLS::Render::Buffers
{
    class Framebuffer;
    class MultiFramebuffer;
    class UniformBuffer;
}

namespace NLS::Render::RHI
{
    struct RHIBindingLayoutDesc;
    struct RHIBindingSetDesc;
    class RHIBindingLayout;
    class RHIBindingSet;
    class RHIGraphicsPipeline;
    class RHIDevice;
    class RHICommandBuffer;
    struct RHIFrameContext;
    class RHIMesh;
}

namespace NLS::Render::Core
{
/**
 * A simple base renderer that doesn't handle any object binding, but provide a strong base for other renderers
 * to implement their own logic.
 */
class NLS_RENDER_API ABaseRenderer : public IRenderer
{
public:
    using PipelineState = Data::PipelineState;
    using Texture2D = Resources::Texture2D;

    struct ExplicitUniformBufferBindingDesc
    {
        uint32_t set = 0u;
        uint32_t registerSpace = 0u;
        uint32_t binding = 0u;
        uint64_t range = 0u;
        RHI::ShaderStageMask stageMask = RHI::ShaderStageMask::AllGraphics;
        std::string entryName;
        std::string layoutDebugName;
        std::string setDebugName;
        std::string snapshotDebugName;
    };

    ABaseRenderer(Context::Driver& p_driver);
    virtual ~ABaseRenderer();

    virtual void BeginFrame(const Data::FrameDescriptor& p_frameDescriptor);
    virtual void EndFrame();

    const Data::FrameDescriptor& GetFrameDescriptor() const;
    PipelineState CreatePipelineState() const;
    bool IsDrawing() const;
    bool SupportsEditorPickingReadback() const;
    bool SupportsFramebufferReadback() const;
    bool IsRenderCoordinateInBounds(uint32_t p_x, uint32_t p_y) const;
    void ReadPixelsFromFramebufferTexture(
        NLS::Render::Buffers::Framebuffer& framebuffer,
        uint32_t p_x,
        uint32_t p_y,
        uint32_t p_width,
        uint32_t p_height,
        Settings::EPixelDataFormat p_format,
        Settings::EPixelDataType p_type,
        void* p_data);
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateExplicitUniformBufferBindingSet(
        const NLS::Render::Buffers::UniformBuffer& buffer,
        const ExplicitUniformBufferBindingDesc& desc) const;
    void BeginLegacyDrawSection();
    void EndLegacyDrawSection();
    bool BeginRecordedRenderPass(
        NLS::Render::Buffers::Framebuffer* p_framebuffer,
        uint16_t p_width,
        uint16_t p_height,
        bool p_clearColor,
        bool p_clearDepth,
        bool p_clearStencil,
        const Maths::Vector4& p_clearValue = Maths::Vector4::Zero);
    bool BeginRecordedRenderPass(
        NLS::Render::Buffers::MultiFramebuffer* p_framebuffer,
        uint16_t p_width,
        uint16_t p_height,
        bool p_clearColor,
        bool p_clearDepth,
        bool p_clearStencil,
        const Maths::Vector4& p_clearValue = Maths::Vector4::Zero);
    bool BeginOutputRenderPass(
        uint16_t p_width,
        uint16_t p_height,
        bool p_clearColor,
        bool p_clearDepth,
        bool p_clearStencil,
        const Maths::Vector4& p_clearValue = Maths::Vector4::Zero,
        bool p_bindLegacyOutputOnFallback = false);
    void EndRecordedRenderPass();
    void EndOutputRenderPass(bool p_startedRecordedPass, bool p_unbindLegacyOutputOnFallback = false);
    void BeginLegacyFramebufferPass(NLS::Render::Buffers::Framebuffer& framebuffer);
    void BeginLegacyFramebufferPass(NLS::Render::Buffers::MultiFramebuffer& framebuffer);
    template<typename TCallback>
    void ExecuteLegacyFramebufferPass(NLS::Render::Buffers::Framebuffer& framebuffer, TCallback&& callback)
    {
        const bool usesExplicitFrame = GetActiveExplicitCommandBuffer() != nullptr;
        if (usesExplicitFrame)
            BeginLegacyDrawSection();
        BeginLegacyFramebufferPass(framebuffer);
        callback();
        EndLegacyFramebufferPass();
        if (usesExplicitFrame)
            EndLegacyDrawSection();
    }
    template<typename TCallback>
    void ExecuteLegacyFramebufferPass(NLS::Render::Buffers::MultiFramebuffer& framebuffer, TCallback&& callback)
    {
        const bool usesExplicitFrame = GetActiveExplicitCommandBuffer() != nullptr;
        if (usesExplicitFrame)
            BeginLegacyDrawSection();
        BeginLegacyFramebufferPass(framebuffer);
        callback();
        EndLegacyFramebufferPass();
        if (usesExplicitFrame)
            EndLegacyDrawSection();
    }
    template<typename TCallback>
    bool ExecuteLegacyFramebufferReadbackPass(
        NLS::Render::Buffers::Framebuffer& framebuffer,
        uint32_t x,
        uint32_t y,
        uint32_t width,
        uint32_t height,
        Settings::EPixelDataFormat format,
        Settings::EPixelDataType type,
        void* data,
        TCallback&& callback)
    {
        if (!SupportsFramebufferReadback() || width == 0u || height == 0u)
            return false;

        const uint64_t maxX = static_cast<uint64_t>(x) + static_cast<uint64_t>(width);
        const uint64_t maxY = static_cast<uint64_t>(y) + static_cast<uint64_t>(height);
        if (maxX > static_cast<uint64_t>(m_frameDescriptor.renderWidth) ||
            maxY > static_cast<uint64_t>(m_frameDescriptor.renderHeight))
        {
            return false;
        }

        const bool usesExplicitFrame = GetActiveExplicitCommandBuffer() != nullptr;
        if (usesExplicitFrame)
            BeginLegacyDrawSection();

        BeginLegacyFramebufferPass(framebuffer);
        callback();

        // Try Formal RHI ReadPixels if explicit device is available
        auto explicitDevice = GetExplicitDevice();
        auto texture = framebuffer.GetExplicitTextureHandle();
        if (explicitDevice != nullptr && texture != nullptr)
        {
            explicitDevice->ReadPixels(texture, x, y, width, height, format, type, data);
        }
        else
        {
            ReadPixels(x, y, width, height, format, type, data);
        }

        EndLegacyFramebufferPass();

        if (usesExplicitFrame)
            EndLegacyDrawSection();

        return true;
    }
    void EndLegacyFramebufferPass();

    void ReadPixels(
        uint32_t p_x,
        uint32_t p_y,
        uint32_t p_width,
        uint32_t p_height,
        Settings::EPixelDataFormat p_format,
        Settings::EPixelDataType p_type,
        void* p_data) const;

    virtual void Clear(
        bool p_colorBuffer,
        bool p_depthBuffer,
        bool p_stencilBuffer,
        const Maths::Vector4& p_color = Maths::Vector4::Zero);

    virtual void DrawEntity(
        PipelineState p_pso,
        const Entities::Drawable& p_drawable);
    void DrawEntity(
        const Entities::Drawable& p_drawable,
        Resources::MaterialPipelineStateOverrides pipelineOverrides = {},
        Settings::EComparaisonAlgorithm depthCompareOverride = Settings::EComparaisonAlgorithm::LESS);

    /**
     * Record a draw call using Formal RHI directly to the command buffer.
     * This bypasses SubmitMaterialDraw and gives the renderer direct control
     * over command recording.
     * Returns true if the draw was recorded using Formal RHI, false otherwise.
     */
    bool RecordDrawExplicit(
        const Entities::Drawable& p_drawable,
        Resources::MaterialPipelineStateOverrides pipelineOverrides = {},
        Settings::EComparaisonAlgorithm depthCompareOverride = Settings::EComparaisonAlgorithm::LESS);

protected:
    struct PreparedRecordedDraw
    {
        std::shared_ptr<RHI::RHICommandBuffer> commandBuffer;
        std::shared_ptr<RHI::RHIGraphicsPipeline> pipeline;
        std::shared_ptr<RHI::RHIBindingSet> materialBindingSet;
        std::shared_ptr<RHI::RHIMesh> mesh;
        uint32_t instanceCount = 0u;
    };

    virtual bool CanRecordExplicitFrame() const;
    virtual bool PrepareRecordedDraw(
        PipelineState p_pso,
        const Entities::Drawable& p_drawable,
        PreparedRecordedDraw& outDraw) const;
    virtual bool PrepareRecordedDraw(
        const Entities::Drawable& p_drawable,
        Resources::MaterialPipelineStateOverrides pipelineOverrides,
        Settings::EComparaisonAlgorithm depthCompareOverride,
        PreparedRecordedDraw& outDraw) const;
    virtual void BindPreparedGraphicsPipeline(const PreparedRecordedDraw& preparedDraw) const;
    virtual void BindPreparedMaterialBindingSet(const PreparedRecordedDraw& preparedDraw) const;
    virtual void SubmitPreparedDraw(const PreparedRecordedDraw& preparedDraw) const;
    NLS::Render::FrameGraph::FrameGraphExecutionContext CreateFrameGraphExecutionContext() const;
    std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> GetActiveExplicitCommandBuffer() const;
    std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDevice() const;
    bool IsLegacyDrawSectionActive() const;
    void BindLegacyOutputTarget();
    void UnbindLegacyOutputTarget();
    void ReadPixelsFromLegacyFramebuffer(
        NLS::Render::Buffers::Framebuffer& framebuffer,
        uint32_t p_x,
        uint32_t p_y,
        uint32_t p_width,
        uint32_t p_height,
        Settings::EPixelDataFormat p_format,
        Settings::EPixelDataType p_type,
        void* p_data);

    Data::FrameDescriptor m_frameDescriptor;
    Context::Driver& m_driver;
    Texture2D* m_emptyTexture;
    PipelineState m_basePipelineState;
    bool m_isDrawing;
    bool m_recordedRenderPassActive = false;
    uint32_t m_legacyDrawSectionDepth = 0;

private:
    static std::atomic_bool s_isDrawing;
    mutable std::unordered_map<std::string, std::weak_ptr<NLS::Render::RHI::RHIBindingLayout>> m_explicitUniformBindingLayouts;
};
}
