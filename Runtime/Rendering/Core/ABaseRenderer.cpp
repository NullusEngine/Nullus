#include <functional>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <cstring>

#include "Rendering/Core/ABaseRenderer.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Buffers/MultiFramebuffer.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include <Debug/Assertion.h>

namespace NLS::Render::Core
{
std::atomic_bool ABaseRenderer::s_isDrawing{ false };

const Entities::Camera kDefaultCamera;

namespace
{
    Maths::Vector4 ToOpaqueClearColor(const Maths::Vector3& color)
    {
        return { color.x, color.y, color.z, 1.0f };
    }

    bool ShouldLogExplicitDrawDiagnostics()
    {
        static const bool enabled = []()
        {
            if (const char* value = std::getenv("NLS_LOG_RENDER_DRAW_PATH"); value != nullptr)
                return std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0;
            return false;
        }();
        return enabled;
    }
}

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

    if (m_driver.HasExplicitRHI() && CanRecordExplicitFrame())
        m_driver.BeginExplicitFrame(p_frameDescriptor.outputBuffer == nullptr);

    const bool usesExplicitRecording = GetActiveExplicitCommandBuffer() != nullptr;

    if (p_frameDescriptor.outputBuffer && !usesExplicitRecording)
        p_frameDescriptor.outputBuffer->Bind();

    m_basePipelineState = m_driver.CreatePipelineState();
    if (!usesExplicitRecording)
    {
        m_driver.SetViewport(0, 0, p_frameDescriptor.renderWidth, p_frameDescriptor.renderHeight);
        Clear(
            p_frameDescriptor.camera->GetClearColorBuffer(),
            p_frameDescriptor.camera->GetClearDepthBuffer(),
            p_frameDescriptor.camera->GetClearStencilBuffer(),
            ToOpaqueClearColor(p_frameDescriptor.camera->GetClearColor())
        );
    }

    p_frameDescriptor.camera->CacheMatrices(p_frameDescriptor.renderWidth, p_frameDescriptor.renderHeight);

    m_isDrawing = true;
    s_isDrawing.store(true);
}

void ABaseRenderer::EndFrame()
{
    NLS_ASSERT(s_isDrawing, "Cannot call EndFrame() before calling BeginFrame()");

    const bool shouldPresentSwapchain = m_frameDescriptor.outputBuffer == nullptr;
    const bool usesExplicitRecording = m_driver.HasExplicitRHI() && GetActiveExplicitCommandBuffer() != nullptr;
    if (usesExplicitRecording)
        m_driver.EndExplicitFrame(shouldPresentSwapchain);

    if (m_frameDescriptor.outputBuffer && !usesExplicitRecording)
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

bool ABaseRenderer::CanRecordExplicitFrame() const
{
    return true;
}

void ABaseRenderer::BeginLegacyDrawSection()
{
    ++m_legacyDrawSectionDepth;
}

void ABaseRenderer::EndLegacyDrawSection()
{
    NLS_ASSERT(m_legacyDrawSectionDepth > 0, "Cannot end a legacy draw section that was never started.");
    --m_legacyDrawSectionDepth;
}

bool ABaseRenderer::BeginRecordedRenderPass(
    Buffers::Framebuffer* p_framebuffer,
    uint16_t p_width,
    uint16_t p_height,
    bool p_clearColor,
    bool p_clearDepth,
    bool p_clearStencil,
    const Maths::Vector4& p_clearValue)
{
    (void)p_clearStencil;
    auto commandBuffer = GetActiveExplicitCommandBuffer();
    if (commandBuffer == nullptr)
        return false;

    NLS_ASSERT(!m_recordedRenderPassActive, "Cannot begin a recorded render pass while another one is active.");

    NLS::Render::RHI::RHIRenderPassDesc renderPassDesc;
    renderPassDesc.renderArea = { 0, 0, p_width, p_height };
    renderPassDesc.debugName = p_framebuffer != nullptr ? "FramebufferRenderPass" : "BackbufferRenderPass";

    NLS::Render::RHI::RHIRenderPassColorAttachmentDesc colorAttachment;
    colorAttachment.loadOp = p_clearColor ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
    colorAttachment.storeOp = NLS::Render::RHI::StoreOp::Store;
    colorAttachment.clearValue = { p_clearValue.x, p_clearValue.y, p_clearValue.z, p_clearValue.w };
    if (p_framebuffer != nullptr)
        colorAttachment.view = p_framebuffer->GetOrCreateExplicitColorView("FramebufferColorView");
    renderPassDesc.colorAttachments.push_back(std::move(colorAttachment));

    if (p_clearDepth || (p_framebuffer != nullptr && p_framebuffer->GetDepthStencilTextureResource() != nullptr))
    {
        NLS::Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthAttachment;
        depthAttachment.depthLoadOp = p_clearDepth ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
        depthAttachment.depthStoreOp = NLS::Render::RHI::StoreOp::Store;
        depthAttachment.stencilLoadOp = NLS::Render::RHI::LoadOp::DontCare;
        depthAttachment.stencilStoreOp = NLS::Render::RHI::StoreOp::DontCare;
        depthAttachment.clearValue.depth = 1.0f;
        if (p_framebuffer != nullptr)
            depthAttachment.view = p_framebuffer->GetOrCreateExplicitDepthStencilView("FramebufferDepthView");
        renderPassDesc.depthStencilAttachment = std::move(depthAttachment);
    }

    commandBuffer->BeginRenderPass(renderPassDesc);
    commandBuffer->SetViewport({ 0.0f, 0.0f, static_cast<float>(p_width), static_cast<float>(p_height), 0.0f, 1.0f });
    m_recordedRenderPassActive = true;
    return true;
}

bool ABaseRenderer::BeginRecordedRenderPass(
    Buffers::MultiFramebuffer* p_framebuffer,
    uint16_t p_width,
    uint16_t p_height,
    bool p_clearColor,
    bool p_clearDepth,
    bool p_clearStencil,
    const Maths::Vector4& p_clearValue)
{
    (void)p_clearStencil;
    auto commandBuffer = GetActiveExplicitCommandBuffer();
    if (commandBuffer == nullptr || p_framebuffer == nullptr)
        return false;

    NLS_ASSERT(!m_recordedRenderPassActive, "Cannot begin a recorded render pass while another one is active.");

    NLS::Render::RHI::RHIRenderPassDesc renderPassDesc;
    renderPassDesc.renderArea = { 0, 0, p_width, p_height };
    renderPassDesc.debugName = "MultiFramebufferRenderPass";

    const auto& colorResources = p_framebuffer->GetColorTextureResources();
    for (size_t i = 0; i < colorResources.size(); ++i)
    {
        NLS::Render::RHI::RHIRenderPassColorAttachmentDesc colorAttachment;
        colorAttachment.loadOp = p_clearColor ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
        colorAttachment.storeOp = NLS::Render::RHI::StoreOp::Store;
        colorAttachment.clearValue = { p_clearValue.x, p_clearValue.y, p_clearValue.z, p_clearValue.w };
        colorAttachment.view = p_framebuffer->GetOrCreateExplicitColorView(i, "MultiFramebufferColorView" + std::to_string(i));
        renderPassDesc.colorAttachments.push_back(std::move(colorAttachment));
    }

    if (p_clearDepth || p_framebuffer->GetDepthTextureResource() != nullptr)
    {
        NLS::Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthAttachment;
        depthAttachment.depthLoadOp = p_clearDepth ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
        depthAttachment.depthStoreOp = NLS::Render::RHI::StoreOp::Store;
        depthAttachment.stencilLoadOp = NLS::Render::RHI::LoadOp::DontCare;
        depthAttachment.stencilStoreOp = NLS::Render::RHI::StoreOp::DontCare;
        depthAttachment.clearValue.depth = 1.0f;
        depthAttachment.view = p_framebuffer->GetOrCreateExplicitDepthView("MultiFramebufferDepthView");
        renderPassDesc.depthStencilAttachment = std::move(depthAttachment);
    }

    commandBuffer->BeginRenderPass(renderPassDesc);
    commandBuffer->SetViewport({ 0.0f, 0.0f, static_cast<float>(p_width), static_cast<float>(p_height), 0.0f, 1.0f });
    m_recordedRenderPassActive = true;
    return true;
}

void ABaseRenderer::EndRecordedRenderPass()
{
    auto commandBuffer = GetActiveExplicitCommandBuffer();
    if (commandBuffer == nullptr || !m_recordedRenderPassActive)
        return;

    commandBuffer->EndRenderPass();
    m_recordedRenderPassActive = false;
}

std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> ABaseRenderer::GetActiveExplicitCommandBuffer() const
{
    const auto* frameContext = m_driver.GetCurrentExplicitFrameContext();
    return frameContext != nullptr ? frameContext->commandBuffer : nullptr;
}

Context::Driver& ABaseRenderer::GetDriver() const
{
    return m_driver;
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

    if (material == nullptr || mesh == nullptr)
        return;

    const auto gpuInstances = material->GetGPUInstances();

	if (mesh && gpuInstances > 0)
	{
        const auto commandBuffer = GetActiveExplicitCommandBuffer();
        if (commandBuffer != nullptr && m_legacyDrawSectionDepth == 0)
        {
            const auto explicitDevice = m_driver.GetExplicitDevice();
            const auto nativeBackend = explicitDevice != nullptr
                ? explicitDevice->GetNativeDeviceInfo().backend
                : NLS::Render::RHI::NativeBackendType::None;
            const auto explicitBindingSet = material->GetExplicitBindingSet(explicitDevice);
            const auto pipelineLayout = material->GetExplicitPipelineLayout(explicitDevice);
            const auto vertexShader = material->GetShader() != nullptr
                ? material->GetShader()->GetOrCreateExplicitShaderModule(explicitDevice, NLS::Render::ShaderCompiler::ShaderStage::Vertex)
                : nullptr;
            const auto fragmentShader = material->GetShader() != nullptr
                ? material->GetShader()->GetOrCreateExplicitShaderModule(explicitDevice, NLS::Render::ShaderCompiler::ShaderStage::Pixel)
                : nullptr;
            const auto explicitPipeline = explicitDevice != nullptr && pipelineLayout != nullptr && vertexShader != nullptr && fragmentShader != nullptr
                ? explicitDevice->CreateGraphicsPipeline(material->BuildExplicitGraphicsPipelineDesc(
                    pipelineLayout,
                    vertexShader,
                    fragmentShader,
                    p_drawable.primitiveMode,
                    p_pso.depthFunc))
                : nullptr;
            const auto vertexBufferView = mesh->GetVertexBufferView();
            if (explicitPipeline != nullptr && explicitBindingSet != nullptr && vertexBufferView.explicitBuffer != nullptr)
            {
                commandBuffer->BindGraphicsPipeline(explicitPipeline);
                commandBuffer->BindBindingSet(1u, explicitBindingSet);
                commandBuffer->BindVertexBuffer(
                    0u,
                    {
                        vertexBufferView.explicitBuffer,
                        static_cast<uint64_t>(vertexBufferView.offset),
                        static_cast<uint32_t>(vertexBufferView.stride)
                    });

                const auto indexBufferView = mesh->GetIndexBufferView();
                if (indexBufferView.has_value() && indexBufferView->explicitBuffer != nullptr && mesh->GetIndexCount() > 0)
                {
                    commandBuffer->BindIndexBuffer(
                        {
                            indexBufferView->explicitBuffer,
                            static_cast<uint64_t>(indexBufferView->offset),
                            NLS::Render::RHI::IndexType::UInt32
                        });
                    commandBuffer->DrawIndexed(mesh->GetIndexCount(), gpuInstances);
                }
                else
                {
                    commandBuffer->Draw(mesh->GetVertexCount(), gpuInstances);
                }
                return;
            }

            if (nativeBackend != NLS::Render::RHI::NativeBackendType::OpenGL)
            {
                if (ShouldLogExplicitDrawDiagnostics())
                {
                    NLS_LOG_ERROR(
                        "[ExplicitDrawPath] Unable to build native explicit draw state for backend " +
                        std::to_string(static_cast<int>(nativeBackend)) +
                        " material=\"" + (material->path.empty() ? std::string("<unnamed>") : material->path) + "\"");
                }
                return;
            }

            if (ShouldLogExplicitDrawDiagnostics())
            {
                NLS_LOG_WARNING(
                    "[ExplicitDrawPath] Falling back to legacy draw for material \"" +
                    (material->path.empty() ? std::string("<unnamed>") : material->path) +
                    "\" device=" + (explicitDevice != nullptr ? "ok" : "null") +
                    " bindingSet=" + (explicitBindingSet != nullptr ? "ok" : "null") +
                    " pipelineLayout=" + (pipelineLayout != nullptr ? "ok" : "null") +
                    " vertexShader=" + (vertexShader != nullptr ? "ok" : "null") +
                    " fragmentShader=" + (fragmentShader != nullptr ? "ok" : "null") +
                    " pipeline=" + (explicitPipeline != nullptr ? "ok" : "null") +
                    " vertexBuffer=" + (vertexBufferView.explicitBuffer != nullptr ? "ok" : "null"));
            }
        }

		auto pipelineDesc = material->BuildGraphicsPipelineDesc();
		pipelineDesc.primitiveMode = p_drawable.primitiveMode;
		p_pso.depthWriting = pipelineDesc.depthStencilState.depthWrite;
        p_pso.colorWriting.mask = pipelineDesc.blendState.colorWrite ? 0xFF : 0x00;
        p_pso.blending = pipelineDesc.blendState.enabled;
        p_pso.culling = pipelineDesc.rasterState.culling;
        p_pso.depthTest = pipelineDesc.depthStencilState.depthTest;

        if (p_pso.culling)
            p_pso.cullFace = pipelineDesc.rasterState.cullFace;

		// Preserve pass-level depth overrides such as skybox LESS_EQUAL while
		// still baking the final compare op into backend pipeline creation.
		pipelineDesc.depthStencilState.depthCompare = p_pso.depthFunc;

        m_driver.BindGraphicsPipeline(pipelineDesc, &material->GetBindingSetInstance());
        m_driver.Draw(p_pso, *mesh, p_drawable.primitiveMode, gpuInstances);
	}
}
}
