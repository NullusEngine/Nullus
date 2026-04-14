#include <functional>
#include <fstream>
#include <filesystem>
#include "Rendering/Core/ABaseRenderer.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Buffers/MultiFramebuffer.h"
#include "Rendering/Buffers/UniformBuffer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHICommand.h"
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

    Resources::MaterialPipelineStateOverrides BuildMaterialPipelineStateOverrides(
        const Data::PipelineState& pipelineState,
        const Resources::Material& material)
    {
        Resources::MaterialPipelineStateOverrides overrides;

        if (pipelineState.depthWriting != material.HasDepthWriting())
            overrides.depthWrite = pipelineState.depthWriting;

        const bool colorWriteEnabled = pipelineState.colorWriting.mask != 0x00;
        if (colorWriteEnabled != material.HasColorWriting())
            overrides.colorWrite = colorWriteEnabled;

        if (pipelineState.depthTest != material.HasDepthTest())
            overrides.depthTest = pipelineState.depthTest;

        const bool materialCullingEnabled = material.HasBackfaceCulling() || material.HasFrontfaceCulling();
        const auto materialCullFace = material.HasBackfaceCulling() && material.HasFrontfaceCulling()
            ? Settings::ECullFace::FRONT_AND_BACK
            : material.HasFrontfaceCulling() ? Settings::ECullFace::FRONT : Settings::ECullFace::BACK;

        if (pipelineState.culling != materialCullingEnabled)
            overrides.culling = pipelineState.culling;

        if (pipelineState.culling && (!materialCullingEnabled || pipelineState.cullFace != materialCullFace))
            overrides.cullFace = pipelineState.cullFace;

        return overrides;
    }

    std::string BuildExplicitUniformBindingLayoutCacheKey(
        const ABaseRenderer::ExplicitUniformBufferBindingDesc& desc)
    {
        return std::to_string(desc.set) + "|" +
            std::to_string(desc.registerSpace) + "|" +
            std::to_string(desc.binding) + "|" +
            std::to_string(static_cast<uint32_t>(desc.stageMask)) + "|" +
            desc.entryName + "|" +
            desc.layoutDebugName;
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

    if (Context::DriverRendererAccess::HasExplicitRHI(m_driver) && CanRecordExplicitFrame())
        Context::DriverRendererAccess::BeginExplicitFrame(m_driver, p_frameDescriptor.outputBuffer == nullptr);

    const bool usesExplicitRecording = GetActiveExplicitCommandBuffer() != nullptr;

    if (p_frameDescriptor.outputBuffer && !usesExplicitRecording)
        p_frameDescriptor.outputBuffer->Bind();

    m_basePipelineState = Context::DriverRendererAccess::CreatePipelineState(m_driver);
    if (!usesExplicitRecording)
    {
        Context::DriverRendererAccess::SetViewport(
            m_driver,
            0,
            0,
            p_frameDescriptor.renderWidth,
            p_frameDescriptor.renderHeight);
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
    const bool usesExplicitRecording =
        Context::DriverRendererAccess::HasExplicitRHI(m_driver) && GetActiveExplicitCommandBuffer() != nullptr;
    if (usesExplicitRecording && m_frameDescriptor.outputBuffer != nullptr)
    {
        Context::DriverRendererAccess::TransitionTextureToShaderRead(
            m_driver,
            m_frameDescriptor.outputBuffer->GetExplicitTextureHandle());
    }
    if (usesExplicitRecording)
        Context::DriverRendererAccess::EndExplicitFrame(m_driver, shouldPresentSwapchain);

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

NLS::Render::FrameGraph::FrameGraphExecutionContext ABaseRenderer::CreateFrameGraphExecutionContext() const
{
    return Context::DriverRendererAccess::CreateFrameGraphExecutionContext(m_driver);
}

bool ABaseRenderer::IsLegacyDrawSectionActive() const
{
    return m_legacyDrawSectionDepth > 0;
}

void ABaseRenderer::BindLegacyOutputTarget()
{
    if (m_frameDescriptor.outputBuffer != nullptr)
        m_frameDescriptor.outputBuffer->Bind();
    else
        Context::DriverRendererAccess::BindDefaultCompatibilityFramebuffer(m_driver);

    Context::DriverRendererAccess::SetViewport(
        m_driver,
        0,
        0,
        m_frameDescriptor.renderWidth,
        m_frameDescriptor.renderHeight);
}

void ABaseRenderer::UnbindLegacyOutputTarget()
{
    if (m_frameDescriptor.outputBuffer != nullptr)
        m_frameDescriptor.outputBuffer->Unbind();
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
    {
        colorAttachment.view = p_framebuffer->GetOrCreateExplicitColorView("FramebufferColorView");
        renderPassDesc.colorAttachments.push_back(std::move(colorAttachment));
    }
    else
    {
        // When no framebuffer (swapchain rendering), use the swapchain backbuffer view
        auto swapchainView = Context::DriverRendererAccess::GetSwapchainBackbufferView(m_driver);
        if (swapchainView != nullptr)
        {
            colorAttachment.view = swapchainView;
            renderPassDesc.colorAttachments.push_back(std::move(colorAttachment));
        }
    }

    if (p_clearDepth || (p_framebuffer != nullptr && p_framebuffer->GetExplicitDepthStencilTextureHandle() != nullptr))
    {
        NLS::Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthAttachment;
        depthAttachment.depthLoadOp = p_clearDepth ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
        depthAttachment.depthStoreOp = NLS::Render::RHI::StoreOp::Store;
        depthAttachment.stencilLoadOp = p_clearStencil ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
        depthAttachment.stencilStoreOp = NLS::Render::RHI::StoreOp::Store;
        depthAttachment.clearValue.depth = 1.0f;
        depthAttachment.clearValue.stencil = 0u;
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

    if (p_clearDepth || p_framebuffer->GetExplicitDepthTextureHandle() != nullptr)
    {
        NLS::Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthAttachment;
        depthAttachment.depthLoadOp = p_clearDepth ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
        depthAttachment.depthStoreOp = NLS::Render::RHI::StoreOp::Store;
        depthAttachment.stencilLoadOp = p_clearStencil ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
        depthAttachment.stencilStoreOp = NLS::Render::RHI::StoreOp::Store;
        depthAttachment.clearValue.depth = 1.0f;
        depthAttachment.clearValue.stencil = 0u;
        depthAttachment.view = p_framebuffer->GetOrCreateExplicitDepthView("MultiFramebufferDepthView");
        renderPassDesc.depthStencilAttachment = std::move(depthAttachment);
    }

    commandBuffer->BeginRenderPass(renderPassDesc);
    commandBuffer->SetViewport({ 0.0f, 0.0f, static_cast<float>(p_width), static_cast<float>(p_height), 0.0f, 1.0f });
    m_recordedRenderPassActive = true;
    return true;
}

bool ABaseRenderer::BeginOutputRenderPass(
    uint16_t p_width,
    uint16_t p_height,
    bool p_clearColor,
    bool p_clearDepth,
    bool p_clearStencil,
    const Maths::Vector4& p_clearValue,
    const bool p_bindLegacyOutputOnFallback)
{
    const bool startedRecordedPass = BeginRecordedRenderPass(
        m_frameDescriptor.outputBuffer,
        p_width,
        p_height,
        p_clearColor,
        p_clearDepth,
        p_clearStencil,
        p_clearValue);
    if (!startedRecordedPass && p_bindLegacyOutputOnFallback)
        BindLegacyOutputTarget();
    return startedRecordedPass;
}

void ABaseRenderer::EndRecordedRenderPass()
{
    auto commandBuffer = GetActiveExplicitCommandBuffer();
    if (commandBuffer == nullptr || !m_recordedRenderPassActive)
        return;

    commandBuffer->EndRenderPass();
    m_recordedRenderPassActive = false;
}

void ABaseRenderer::EndOutputRenderPass(const bool p_startedRecordedPass, const bool p_unbindLegacyOutputOnFallback)
{
    if (p_startedRecordedPass)
    {
        EndRecordedRenderPass();
        return;
    }

    if (p_unbindLegacyOutputOnFallback)
        UnbindLegacyOutputTarget();
}

void ABaseRenderer::BeginLegacyFramebufferPass(Buffers::Framebuffer& framebuffer)
{
    framebuffer.Bind();
    Context::DriverRendererAccess::SetViewport(
        m_driver,
        0,
        0,
        m_frameDescriptor.renderWidth,
        m_frameDescriptor.renderHeight);
}

void ABaseRenderer::BeginLegacyFramebufferPass(Buffers::MultiFramebuffer& framebuffer)
{
    framebuffer.Bind();
    Context::DriverRendererAccess::SetViewport(
        m_driver,
        0,
        0,
        m_frameDescriptor.renderWidth,
        m_frameDescriptor.renderHeight);
}

void ABaseRenderer::EndLegacyFramebufferPass()
{
    BindLegacyOutputTarget();
}

void ABaseRenderer::ReadPixelsFromLegacyFramebuffer(
    Buffers::Framebuffer& framebuffer,
    const uint32_t p_x,
    const uint32_t p_y,
    const uint32_t p_width,
    const uint32_t p_height,
    const Settings::EPixelDataFormat p_format,
    const Settings::EPixelDataType p_type,
    void* p_data)
{
    BeginLegacyFramebufferPass(framebuffer);
    ReadPixels(p_x, p_y, p_width, p_height, p_format, p_type, p_data);
    EndLegacyFramebufferPass();
}

std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> ABaseRenderer::GetActiveExplicitCommandBuffer() const
{
    return Context::DriverRendererAccess::GetActiveExplicitCommandBuffer(m_driver);
}

std::shared_ptr<NLS::Render::RHI::RHIDevice> ABaseRenderer::GetExplicitDevice() const
{
    return Context::DriverRendererAccess::GetExplicitDevice(m_driver);
}

bool ABaseRenderer::SupportsEditorPickingReadback() const
{
    return Context::DriverRendererAccess::SupportsEditorPickingReadback(m_driver);
}

bool ABaseRenderer::SupportsFramebufferReadback() const
{
    return Context::DriverRendererAccess::SupportsFramebufferReadback(m_driver);
}

std::shared_ptr<NLS::Render::RHI::RHIBindingSet> ABaseRenderer::CreateExplicitUniformBufferBindingSet(
    const NLS::Render::Buffers::UniformBuffer& buffer,
    const ExplicitUniformBufferBindingDesc& desc) const
{
    const auto cacheKey = BuildExplicitUniformBindingLayoutCacheKey(desc);

    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> layout;
    if (const auto found = m_explicitUniformBindingLayouts.find(cacheKey);
        found != m_explicitUniformBindingLayouts.end())
    {
        layout = found->second.lock();
    }

    if (layout == nullptr)
    {
        NLS::Render::RHI::RHIBindingLayoutDesc layoutDesc;
        layoutDesc.debugName = desc.layoutDebugName;
        layoutDesc.entries.push_back({
            desc.entryName,
            NLS::Render::RHI::BindingType::UniformBuffer,
            desc.set,
            desc.binding,
            1u,
            desc.stageMask,
            desc.registerSpace
        });
        layout = Context::DriverRendererAccess::CreateExplicitBindingLayout(m_driver, layoutDesc);
        if (layout != nullptr)
            m_explicitUniformBindingLayouts[cacheKey] = layout;
    }

    if (layout == nullptr)
        return nullptr;

    const auto snapshotBuffer = buffer.CreateExplicitSnapshotBuffer(desc.snapshotDebugName);
    const auto bufferHandle = snapshotBuffer != nullptr
        ? snapshotBuffer
        : buffer.GetExplicitRHIBufferHandle();
    if (bufferHandle == nullptr)
        return nullptr;

    NLS::Render::RHI::RHIBindingSetDesc bindingSetDesc;
    bindingSetDesc.layout = layout;
    bindingSetDesc.debugName = desc.setDebugName;
    bindingSetDesc.entries.push_back({
        desc.binding,
        NLS::Render::RHI::BindingType::UniformBuffer,
        bufferHandle,
        0u,
        desc.range != 0u ? desc.range : bufferHandle->GetDesc().size,
        nullptr,
        nullptr
    });
    return Context::DriverRendererAccess::CreateExplicitBindingSet(m_driver, bindingSetDesc);
}

bool ABaseRenderer::IsRenderCoordinateInBounds(uint32_t p_x, uint32_t p_y) const
{
    return p_x < m_frameDescriptor.renderWidth && p_y < m_frameDescriptor.renderHeight;
}

void ABaseRenderer::ReadPixelsFromFramebufferTexture(
    Buffers::Framebuffer& framebuffer,
    uint32_t p_x,
    uint32_t p_y,
    uint32_t p_width,
    uint32_t p_height,
    Settings::EPixelDataFormat p_format,
    Settings::EPixelDataType p_type,
    void* p_data)
{
    auto explicitDevice = GetExplicitDevice();
    auto texture = framebuffer.GetExplicitTextureHandle();
    if (explicitDevice != nullptr && texture != nullptr)
    {
        explicitDevice->ReadPixels(texture, p_x, p_y, p_width, p_height, p_format, p_type, p_data);
        return;
    }

    ReadPixelsFromLegacyFramebuffer(framebuffer, p_x, p_y, p_width, p_height, p_format, p_type, p_data);
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
    Context::DriverRendererAccess::ReadPixels(
        m_driver,
        p_x,
        p_y,
        p_width,
        p_height,
        p_format,
        p_type,
        p_data);
}

void ABaseRenderer::Clear(
    bool p_colorBuffer,
    bool p_depthBuffer,
    bool p_stencilBuffer,
    const Maths::Vector4& p_color
)
{
    Context::DriverRendererAccess::Clear(m_driver, p_colorBuffer, p_depthBuffer, p_stencilBuffer, p_color);
}

bool ABaseRenderer::PrepareRecordedDraw(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable,
    PreparedRecordedDraw& outDraw) const
{
    auto material = p_drawable.material;
    if (material == nullptr)
        return false;

    const auto pipelineOverrides = BuildMaterialPipelineStateOverrides(p_pso, *material);

    auto mesh = p_drawable.mesh;
    if (mesh == nullptr)
        return false;

    const auto gpuInstances = material->GetGPUInstances();
    if (gpuInstances <= 0)
        return false;

    auto commandBuffer = GetActiveExplicitCommandBuffer();
    auto device = GetExplicitDevice();
    auto pipeline = material->BuildRecordedGraphicsPipeline(
        device,
        p_drawable.primitiveMode,
        p_pso,
        pipelineOverrides);
    auto bindingSet = material->GetRecordedBindingSet(device);
    auto rhiMesh = mesh->GetRHIMesh();

    if (commandBuffer == nullptr || pipeline == nullptr || bindingSet == nullptr || rhiMesh == nullptr)
        return false;

    outDraw.commandBuffer = std::move(commandBuffer);
    outDraw.pipeline = std::move(pipeline);
    outDraw.materialBindingSet = std::move(bindingSet);
    outDraw.mesh = std::move(rhiMesh);
    outDraw.instanceCount = gpuInstances;
    return true;
}

bool ABaseRenderer::PrepareRecordedDraw(
    const Entities::Drawable& p_drawable,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride,
    PreparedRecordedDraw& outDraw) const
{
    auto material = p_drawable.material;
    auto mesh = p_drawable.mesh;

    if (material == nullptr || mesh == nullptr)
        return false;

    const auto gpuInstances = material->GetGPUInstances();
    if (!(mesh && gpuInstances > 0))
        return false;

    auto commandBuffer = GetActiveExplicitCommandBuffer();
    auto device = GetExplicitDevice();
    auto effectivePipelineState = CreatePipelineState();
    effectivePipelineState.depthFunc = depthCompareOverride;
    auto pipeline = material->BuildRecordedGraphicsPipeline(
        device, p_drawable.primitiveMode, effectivePipelineState, pipelineOverrides);
    auto bindingSet = material->GetRecordedBindingSet(device);
    auto rhiMesh = mesh->GetRHIMesh();

    if (commandBuffer == nullptr || pipeline == nullptr || bindingSet == nullptr || rhiMesh == nullptr)
        return false;

    outDraw.commandBuffer = std::move(commandBuffer);
    outDraw.pipeline = std::move(pipeline);
    outDraw.materialBindingSet = std::move(bindingSet);
    outDraw.mesh = std::move(rhiMesh);
    outDraw.instanceCount = gpuInstances;
    return true;
}

void ABaseRenderer::BindPreparedGraphicsPipeline(const PreparedRecordedDraw& preparedDraw) const
{
    if (preparedDraw.commandBuffer != nullptr && preparedDraw.pipeline != nullptr)
        preparedDraw.commandBuffer->BindGraphicsPipeline(preparedDraw.pipeline);
}

void ABaseRenderer::BindPreparedMaterialBindingSet(const PreparedRecordedDraw& preparedDraw) const
{
    if (preparedDraw.commandBuffer != nullptr && preparedDraw.materialBindingSet != nullptr)
    {
        preparedDraw.commandBuffer->BindBindingSet(
            NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet,
            preparedDraw.materialBindingSet);
    }
}

void ABaseRenderer::SubmitPreparedDraw(const PreparedRecordedDraw& preparedDraw) const
{
    if (preparedDraw.commandBuffer == nullptr || preparedDraw.mesh == nullptr)
        return;

    const auto vertexBuffer = preparedDraw.mesh->GetVertexBuffer();
    if (vertexBuffer == nullptr)
        return;

    preparedDraw.commandBuffer->BindVertexBuffer(
        0,
        { vertexBuffer, 0, preparedDraw.mesh->GetVertexStride() });

    const auto indexBuffer = preparedDraw.mesh->GetIndexBuffer();
    const auto indexCount = preparedDraw.mesh->GetIndexCount();
    if (indexBuffer != nullptr && indexCount > 0u)
    {
        preparedDraw.commandBuffer->BindIndexBuffer(
            { indexBuffer, 0, preparedDraw.mesh->GetIndexType() });
        preparedDraw.commandBuffer->DrawIndexed(
            indexCount,
            preparedDraw.instanceCount,
            0,
            0,
            0);
        return;
    }

    const auto vertexCount = preparedDraw.mesh->GetVertexCount();
    if (vertexCount > 0u)
        preparedDraw.commandBuffer->Draw(vertexCount, preparedDraw.instanceCount, 0, 0);
}

void ABaseRenderer::DrawEntity(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable
)
{
    PreparedRecordedDraw preparedDraw;
    if (!PrepareRecordedDraw(p_pso, p_drawable, preparedDraw))
        return;

    BindPreparedGraphicsPipeline(preparedDraw);
    BindPreparedMaterialBindingSet(preparedDraw);
    SubmitPreparedDraw(preparedDraw);
}

void ABaseRenderer::DrawEntity(
    const Entities::Drawable& p_drawable,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride)
{
    auto material = p_drawable.material;
    auto mesh = p_drawable.mesh;

    if (material == nullptr || mesh == nullptr)
    {
        NLS_LOG_ERROR("[ABaseRenderer] DrawEntity: material or mesh is null!");
        return;
    }

    PreparedRecordedDraw preparedDraw;
    if (!PrepareRecordedDraw(p_drawable, pipelineOverrides, depthCompareOverride, preparedDraw))
        return;

    BindPreparedGraphicsPipeline(preparedDraw);
    BindPreparedMaterialBindingSet(preparedDraw);
    SubmitPreparedDraw(preparedDraw);
}
}
