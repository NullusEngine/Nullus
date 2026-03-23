#include <functional>

#include "Rendering/Core/ABaseRenderer.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include <Debug/Assertion.h>

namespace NLS::Render::Core
{
std::atomic_bool ABaseRenderer::s_isDrawing{ false };

const Entities::Camera kDefaultCamera;

ABaseRenderer::ABaseRenderer(Context::Driver& p_driver) :
    m_driver(p_driver),
    m_isDrawing(false),
    m_emptyTexture(Resources::Loaders::TextureLoader::CreatePixel(255, 255, 255, 255))
{
}

ABaseRenderer::~ABaseRenderer()
{
    Resources::Loaders::TextureLoader::Destroy(m_emptyTexture);
}

void ABaseRenderer::BeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
    NLS_ASSERT(!s_isDrawing, "Cannot call BeginFrame() when previous frame hasn't finished.");
    NLS_ASSERT(p_frameDescriptor.IsValid(), "Invalid FrameDescriptor!");

    m_frameDescriptor = p_frameDescriptor;

    if (p_frameDescriptor.outputBuffer)
    {
        p_frameDescriptor.outputBuffer->Bind();
    }

    m_basePipelineState = m_driver.CreatePipelineState();
    m_driver.SetViewport(0, 0, p_frameDescriptor.renderWidth, p_frameDescriptor.renderHeight);

    Clear(
        p_frameDescriptor.camera->GetClearColorBuffer(),
        p_frameDescriptor.camera->GetClearDepthBuffer(),
        p_frameDescriptor.camera->GetClearStencilBuffer(),
        p_frameDescriptor.camera->GetClearColor()
    );

    p_frameDescriptor.camera->CacheMatrices(p_frameDescriptor.renderWidth, p_frameDescriptor.renderHeight);

    m_isDrawing = true;
    s_isDrawing.store(true);
}

void ABaseRenderer::EndFrame()
{
    NLS_ASSERT(s_isDrawing, "Cannot call EndFrame() before calling BeginFrame()");

    if (m_frameDescriptor.outputBuffer)
    {
        m_frameDescriptor.outputBuffer->Unbind();
    }

    m_isDrawing = false;
    s_isDrawing.store(false);
}

const Data::FrameDescriptor& ABaseRenderer::GetFrameDescriptor() const
{
    NLS_ASSERT(m_isDrawing, "Cannot call GetFrameDescriptor() outside of a frame");
    return m_frameDescriptor;
}

ABaseRenderer::PipelineState ABaseRenderer::CreatePipelineState() const
{
    return m_basePipelineState;
}

bool ABaseRenderer::IsDrawing() const
{
    return m_isDrawing;
}

void ABaseRenderer::ReadPixels(
    uint32_t p_x,
    uint32_t p_y,
    uint32_t p_width,
    uint32_t p_height,
    Settings::EPixelDataFormat p_format,
    Settings::EPixelDataType p_type,
    void* p_data
) const
{
    return m_driver.ReadPixels(p_x, p_y, p_width, p_height, p_format, p_type, p_data);
}

void ABaseRenderer::Clear(
    bool p_colorBuffer,
    bool p_depthBuffer,
    bool p_stencilBuffer,
    const Maths::Vector4& p_color
)
{
    m_driver.Clear(p_colorBuffer, p_depthBuffer, p_stencilBuffer, p_color);
}

void ABaseRenderer::DrawEntity(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable
)
{
    auto material = p_drawable.material;
    auto mesh = p_drawable.mesh;

    const auto gpuInstances = material->GetGPUInstances();

    if (mesh && gpuInstances > 0)
    {
        const auto pipelineDesc = material->BuildGraphicsPipelineDesc();
        p_pso.depthWriting = pipelineDesc.depthStencilState.depthWrite;
        p_pso.colorWriting.mask = pipelineDesc.blendState.colorWrite ? 0xFF : 0x00;
        p_pso.blending = pipelineDesc.blendState.enabled;
        p_pso.culling = pipelineDesc.rasterState.culling;
        p_pso.depthTest = pipelineDesc.depthStencilState.depthTest;
        p_pso.depthFunc = pipelineDesc.depthStencilState.depthCompare;

        if (p_pso.culling)
        {
            p_pso.cullFace = pipelineDesc.rasterState.cullFace;
        }

        material->Bind(m_emptyTexture);
        m_driver.Draw(p_pso, *mesh, p_drawable.primitiveMode, gpuInstances);
        material->UnBind();
    }
}
}
