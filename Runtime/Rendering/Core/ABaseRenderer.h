#pragma once

#include <atomic>

#include "Rendering/Core/IRenderer.h"
#include "Rendering/Data/FrameInfo.h"
#include "Rendering/Resources/IMesh.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Entities/Drawable.h"
#include "RenderDef.h"

namespace NLS::Render::Buffers
{
    class Framebuffer;
    class MultiFramebuffer;
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

    ABaseRenderer(Context::Driver& p_driver);
    virtual ~ABaseRenderer();

    virtual void BeginFrame(const Data::FrameDescriptor& p_frameDescriptor);
    virtual void EndFrame();

    const Data::FrameDescriptor& GetFrameDescriptor() const;
    PipelineState CreatePipelineState() const;
    bool IsDrawing() const;
    Context::Driver& GetDriver() const;
    std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> GetActiveExplicitCommandBuffer() const;
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
    void EndRecordedRenderPass();

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

protected:
    virtual bool CanRecordExplicitFrame() const;

    Data::FrameDescriptor m_frameDescriptor;
    Context::Driver& m_driver;
    Texture2D* m_emptyTexture;
    PipelineState m_basePipelineState;
    bool m_isDrawing;
    bool m_recordedRenderPassActive = false;
    uint32_t m_legacyDrawSectionDepth = 0;

private:
    static std::atomic_bool s_isDrawing;
};
}
