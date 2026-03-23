#pragma once

#include <atomic>

#include "Rendering/Core/IRenderer.h"
#include "Rendering/Data/FrameInfo.h"
#include "Rendering/Resources/IMesh.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Entities/Drawable.h"
#include "RenderDef.h"

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
    Data::FrameDescriptor m_frameDescriptor;
    Context::Driver& m_driver;
    Texture2D* m_emptyTexture;
    PipelineState m_basePipelineState;
    bool m_isDrawing;

private:
    static std::atomic_bool s_isDrawing;
};
}
