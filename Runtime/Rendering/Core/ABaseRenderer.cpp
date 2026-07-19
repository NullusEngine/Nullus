#include <algorithm>
#include <array>
#include <functional>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <sstream>
#include "Profiling/Profiler.h"
#include "Rendering/Core/ABaseRenderer.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Buffers/MultiFramebuffer.h"
#include "Rendering/Buffers/UniformBuffer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/IndexedObjectDataShaderSupport.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Settings/EngineDiagnosticsSettings.h"
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

    Maths::Vector4 ResolveFrameClearColor(const Data::FrameDescriptor& frameDescriptor)
    {
        if (frameDescriptor.clearColorOverride.has_value())
            return *frameDescriptor.clearColorOverride;
        if (frameDescriptor.camera == nullptr)
            return Core::DefaultOpaqueClearColor();
        return ToOpaqueClearColor(frameDescriptor.camera->GetClearColor());
    }

    struct AttachmentPipelineState
    {
        std::optional<RHI::TextureFormat> colorFormat;
        std::optional<RHI::TextureColorSpace> colorSpace;
        std::optional<RHI::TextureFormat> depthFormat;
        std::optional<uint32_t> colorSampleCount;
        std::optional<uint32_t> depthSampleCount;
        bool hasDepthAttachment = false;
    };

    AttachmentPipelineState ResolveAttachmentPipelineState(const Data::FrameDescriptor& frameDescriptor)
    {
        AttachmentPipelineState state;
        auto* outputFramebuffer = NLS::Render::FrameGraph::ResolveExternalSceneOutputFramebuffer(frameDescriptor);
        const auto colorView = frameDescriptor.outputColorView;
        const auto colorTexture =
            colorView != nullptr
                ? colorView->GetTexture()
                : frameDescriptor.outputColorTexture != nullptr
                    ? frameDescriptor.outputColorTexture
                    : outputFramebuffer != nullptr
                        ? outputFramebuffer->GetExplicitTextureHandle()
                        : nullptr;
        const auto depthView = frameDescriptor.outputDepthStencilView;
        const auto depthTexture =
            depthView != nullptr
                ? depthView->GetTexture()
                : frameDescriptor.outputDepthStencilTexture != nullptr
                    ? frameDescriptor.outputDepthStencilTexture
                    : outputFramebuffer != nullptr
                        ? outputFramebuffer->GetExplicitDepthStencilTextureHandle()
                        : nullptr;

        if (colorView != nullptr)
        {
            state.colorFormat = colorView->GetDesc().format;
            state.colorSpace = colorView->GetDesc().colorSpace;
        }
        else if (colorTexture != nullptr)
        {
            state.colorFormat = colorTexture->GetDesc().format;
            state.colorSpace = colorTexture->GetDesc().colorSpace;
        }
        if (colorTexture != nullptr)
            state.colorSampleCount = std::max(1u, colorTexture->GetDesc().sampleCount);

        if (depthView != nullptr)
            state.depthFormat = depthView->GetDesc().format;
        else if (depthTexture != nullptr)
            state.depthFormat = depthTexture->GetDesc().format;
        if (depthTexture != nullptr)
            state.depthSampleCount = std::max(1u, depthTexture->GetDesc().sampleCount);

        state.hasDepthAttachment =
            depthTexture != nullptr ||
            outputFramebuffer != nullptr ||
            NLS::Render::FrameGraph::FrameTargetsSwapchain(frameDescriptor);
        return state;
    }

    std::optional<Resources::MaterialPipelineStateOverrides> BuildMaterialPipelineStateOverrides(
        const Data::PipelineState& pipelineState,
        const Resources::Material& material,
        const Data::FrameDescriptor& frameDescriptor)
    {
        Resources::MaterialPipelineStateOverrides overrides;

        if (pipelineState.depthWriting != material.HasDepthWriting())
            overrides.depthWrite = pipelineState.depthWriting;

        const bool colorWriteEnabled = pipelineState.colorWriting.mask != 0x00;
        if (colorWriteEnabled != material.HasColorWriting())
            overrides.colorWrite = colorWriteEnabled;

        if (pipelineState.depthTest != material.HasDepthTest())
            overrides.depthTest = pipelineState.depthTest;

        const auto attachmentState = ResolveAttachmentPipelineState(frameDescriptor);
        if (attachmentState.colorSampleCount.has_value() &&
            attachmentState.depthSampleCount.has_value() &&
            *attachmentState.colorSampleCount != *attachmentState.depthSampleCount)
        {
            return std::nullopt;
        }

        if (attachmentState.colorFormat.has_value())
        {
            const std::array<RHI::TextureFormat, 1u> colorFormats { *attachmentState.colorFormat };
            overrides.SetColorFormats(colorFormats);
        }
        if (attachmentState.colorSpace.has_value())
        {
            const std::array<RHI::TextureColorSpace, 1u> colorSpaces { *attachmentState.colorSpace };
            overrides.SetColorSpaces(colorSpaces);
        }

        if (attachmentState.colorSampleCount.has_value())
            overrides.sampleCount = *attachmentState.colorSampleCount;

        if (attachmentState.depthFormat.has_value())
        {
            overrides.hasDepthAttachment = true;
            overrides.depthFormat = *attachmentState.depthFormat;
            overrides.sampleCount = attachmentState.depthSampleCount.value_or(overrides.sampleCount.value_or(1u));
        }
        else
        {
            overrides.hasDepthAttachment = attachmentState.hasDepthAttachment;
        }

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

    bool ApplyAttachmentPipelineStateOverrides(
        const Data::FrameDescriptor& frameDescriptor,
        Resources::MaterialPipelineStateOverrides& overrides)
    {
        const auto attachmentState = ResolveAttachmentPipelineState(frameDescriptor);
        if (attachmentState.colorSampleCount.has_value() &&
            attachmentState.depthSampleCount.has_value() &&
            *attachmentState.colorSampleCount != *attachmentState.depthSampleCount)
        {
            return false;
        }

        if (!overrides.HasColorFormatsOverride() && attachmentState.colorFormat.has_value())
        {
            const std::array<RHI::TextureFormat, 1u> colorFormats { *attachmentState.colorFormat };
            overrides.SetColorFormats(colorFormats);
        }

        if (!overrides.HasColorSpacesOverride() && attachmentState.colorSpace.has_value())
        {
            const std::array<RHI::TextureColorSpace, 1u> colorSpaces { *attachmentState.colorSpace };
            overrides.SetColorSpaces(colorSpaces);
        }

        if (!overrides.sampleCount.has_value() && attachmentState.colorSampleCount.has_value())
            overrides.sampleCount = *attachmentState.colorSampleCount;

        if (!overrides.depthFormat.has_value() && attachmentState.depthFormat.has_value())
            overrides.depthFormat = *attachmentState.depthFormat;

        if (!overrides.hasDepthAttachment.has_value())
            overrides.hasDepthAttachment = attachmentState.depthFormat.has_value() || attachmentState.hasDepthAttachment;

        if (attachmentState.depthFormat.has_value())
        {
            overrides.hasDepthAttachment = true;
            if (!overrides.sampleCount.has_value())
                overrides.sampleCount = attachmentState.depthSampleCount.value_or(1u);
        }

        return true;
    }

    template<typename TValue>
    void HashCombine(size_t& seed, const TValue& value)
    {
        seed ^= std::hash<TValue>{}(value) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    }

    size_t HashPipelineState(const Data::PipelineState& pipelineState)
    {
        return std::hash<unsigned long long>{}(pipelineState.bits.to_ullong());
    }

    RHI::NativeBackendType ResolveDeviceBackendType(const std::shared_ptr<RHI::RHIDevice>& device)
    {
        return device != nullptr ? device->GetNativeDeviceInfo().backend : RHI::NativeBackendType::None;
    }

    constexpr size_t kMaxPreparedRecordedDrawStaticBaseCacheEntries = 1024u;
    constexpr uint64_t kMaxPreparedRecordedDrawStaticBaseCacheFrameAge = 120u;
    constexpr size_t kPreparedRecordedDrawStaticBaseCacheAgeSweepBudget = 16u;

    std::string BuildExplicitUniformBindingLayoutCacheKey(
        const ABaseRenderer::ExplicitUniformBufferBindingDesc& desc,
        const std::shared_ptr<RHI::RHIDevice>& device)
    {
        return std::to_string(device != nullptr ? device->GetCacheIdentity() : 0u) + "|" +
            std::to_string(static_cast<uint32_t>(ResolveDeviceBackendType(device))) + "|" +
            std::to_string(desc.set) + "|" +
            std::to_string(desc.registerSpace) + "|" +
            std::to_string(desc.binding) + "|" +
            std::to_string(static_cast<uint32_t>(desc.stageMask)) + "|" +
            desc.entryName + "|" +
            desc.layoutDebugName;
    }

    bool ShouldLogRecordedDrawPreparation(const Context::Driver& driver)
    {
        return Context::DriverRendererAccess::GetDiagnosticsSettings(driver).logRenderDrawPath;
    }

    const char* BoolFlag(const bool value)
    {
        return value ? "1" : "0";
    }

    void AppendTextureVisibilityTransitionBarrier(
        NLS::Render::RHI::RHIBarrierDesc& barriers,
        const NLS::Render::Context::TextureVisibilityTransition& transition)
    {
        barriers.textureBarriers.push_back({
            transition.texture,
            transition.before,
            transition.after,
            transition.subresourceRange,
            transition.sourceStages,
            transition.destinationStages,
            transition.sourceAccess,
            transition.destinationAccess
        });
    }

    void LogRecordedDrawPreparationState(
        const Context::Driver& driver,
        const char* overloadName,
        const char* reason,
        const Entities::Drawable& drawable,
        const std::shared_ptr<RHI::RHIDevice>& device = nullptr,
        const std::shared_ptr<RHI::PipelineCache>& pipelineCache = nullptr,
        const std::shared_ptr<RHI::RHIGraphicsPipeline>& pipeline = nullptr,
        const std::shared_ptr<RHI::RHIBindingSet>& bindingSet = nullptr,
        const std::shared_ptr<RHI::RHIMesh>& rhiMesh = nullptr,
        const bool hasPipelineLayout = false,
        const bool hasVertexShader = false,
        const bool hasFragmentShader = false)
    {
        if (!ShouldLogRecordedDrawPreparation(driver))
            return;

        const auto* material = drawable.material;
        const auto* mesh = drawable.mesh;
        const auto* shader = material != nullptr ? material->GetShader() : nullptr;

        std::ostringstream message;
        message << "[ABaseRenderer] PrepareRecordedDraw(" << overloadName << ") failed"
            << " reason=" << reason
            << " material=" << BoolFlag(material != nullptr)
            << " shader=" << BoolFlag(shader != nullptr)
            << " shaderPath=\"" << (shader != nullptr ? shader->path : std::string{}) << "\""
            << " mesh=" << BoolFlag(mesh != nullptr)
            << " meshVertices=" << (mesh != nullptr ? mesh->GetVertexCount() : 0u)
            << " meshIndices=" << (mesh != nullptr ? mesh->GetIndexCount() : 0u)
            << " gpuInstances=" << (material != nullptr ? material->GetGPUInstances() : 0)
            << " device=" << BoolFlag(device != nullptr)
            << " pipelineCache=" << BoolFlag(pipelineCache != nullptr)
            << " pipelineLayout=" << BoolFlag(hasPipelineLayout)
            << " vertexShader=" << BoolFlag(hasVertexShader)
            << " fragmentShader=" << BoolFlag(hasFragmentShader)
            << " pipeline=" << BoolFlag(pipeline != nullptr)
            << " materialBindingSet=" << BoolFlag(bindingSet != nullptr)
            << " rhiMesh=" << BoolFlag(rhiMesh != nullptr)
            << " vertexBuffer=" << BoolFlag(rhiMesh != nullptr && rhiMesh->GetVertexBuffer() != nullptr)
            << " indexBuffer=" << BoolFlag(rhiMesh != nullptr && rhiMesh->GetIndexBuffer() != nullptr);
        NLS_LOG_INFO(message.str());

        if (material == nullptr)
            return;

        for (const auto& diagnostic : material->GetLastExplicitBindingDiagnostics())
        {
            NLS_LOG_INFO(
                "[ABaseRenderer] PrepareRecordedDraw(" + std::string(overloadName) +
                ") bindingDiagnostic severity=" + std::to_string(static_cast<int>(diagnostic.severity)) +
                " binding=\"" + diagnostic.bindingName +
                "\" message=\"" + diagnostic.message + "\"");
        }
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

void ABaseRenderer::BeginFrameInternal(
    const Data::FrameDescriptor& p_frameDescriptor,
    bool globalFrameAlreadyAcquired)
{
    NLS_PROFILE_SCOPE();
    if (!globalFrameAlreadyAcquired)
    {
        bool expectedDrawing = false;
        const bool beganGlobalFrame = s_isDrawing.compare_exchange_strong(expectedDrawing, true);
        if (!beganGlobalFrame)
        {
            m_isDrawing = false;
            m_frameActive = false;
            m_globalFrameReleaseDeferred = true;
            return;
        }
    }
    NLS_ASSERT(p_frameDescriptor.IsValid(), "Invalid FrameDescriptor!");

    {
        NLS_PROFILE_NAMED_SCOPE("ABaseRenderer::BeginFrameInternal::ResetPreparedFrameState");
        m_frameDescriptor = p_frameDescriptor;
        m_threadedRecordedDrawCommands.clear();
        m_activeRecordedPassColorViews.clear();
        m_activeRecordedPassDepthStencilView.reset();
        m_pendingFrameSnapshot.reset();
        m_pendingPreparedRenderSceneBuilder = {};
        m_lastThreadedFramePublished = false;
        m_frameActive = false;
        m_globalFrameReleaseDeferred = globalFrameAlreadyAcquired;
    }
    {
        NLS_PROFILE_NAMED_SCOPE("ABaseRenderer::BeginFrameInternal::MaintainPreparedDrawCache");
        ++m_preparedRecordedDrawStaticBaseCacheFrame;
        InvalidateExplicitDeviceDependentCachesIfNeeded();
        TrimPreparedRecordedDrawStaticBaseCache(true);
    }
    const bool targetsSwapchain = NLS::Render::FrameGraph::FrameTargetsSwapchain(p_frameDescriptor);
    auto* externalOutputBuffer =
        NLS::Render::FrameGraph::ResolveExternalSceneOutputFramebuffer(p_frameDescriptor);

    const bool usesThreadedRendering = Context::RenderThreadCoordinator::IsThreadedRenderingEnabled(m_driver);
    const bool rendererFrameStarted = Context::RenderThreadCoordinator::BeginRendererFrame(
        m_driver,
        targetsSwapchain);

    const bool usesExplicitRecording = GetActiveExplicitCommandBuffer() != nullptr;
    if (!usesThreadedRendering &&
        Context::DriverRendererAccess::HasExplicitRHI(m_driver) &&
        !rendererFrameStarted)
    {
        return;
    }

    m_basePipelineState = Context::DriverRendererAccess::CreatePipelineState(m_driver);
    if (!usesThreadedRendering && !usesExplicitRecording)
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
            ResolveFrameClearColor(p_frameDescriptor)
        );
    }

    p_frameDescriptor.camera->CacheMatrices(p_frameDescriptor.renderWidth, p_frameDescriptor.renderHeight);

    m_isDrawing = true;
    m_frameActive = true;
}

void ABaseRenderer::BeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
    BeginFrameInternal(p_frameDescriptor, false);
}

void ABaseRenderer::BeginFrameForBackgroundPreview(const Data::FrameDescriptor& p_frameDescriptor)
{
    BeginFrameInternal(p_frameDescriptor, true);
}

void ABaseRenderer::AbortFrameForBackgroundPreview()
{
    m_threadedRecordedDrawCommands.clear();
    m_activeRecordedPassColorViews.clear();
    m_activeRecordedPassDepthStencilView.reset();
    m_pendingFrameSnapshot.reset();
    m_pendingPreparedRenderSceneBuilder = {};
    m_nextFramePostSubmitTextureReadbacks.clear();
    m_isDrawing = false;
    m_frameActive = false;
    m_globalFrameReleaseDeferred = false;
}

void ABaseRenderer::EndFrame()
{
    NLS_PROFILE_SCOPE();
    if (!m_frameActive)
    {
        const bool releaseGlobalFrame = !m_globalFrameReleaseDeferred;
        m_isDrawing = false;
        m_globalFrameReleaseDeferred = false;
        if (releaseGlobalFrame)
            s_isDrawing.store(false);
        return;
    }
    NLS_ASSERT(s_isDrawing, "Cannot call EndFrame() before calling BeginFrame()");

    const bool usesThreadedRendering = Context::RenderThreadCoordinator::IsThreadedRenderingEnabled(m_driver);
    const bool shouldPresentSwapchain = NLS::Render::FrameGraph::FrameTargetsSwapchain(m_frameDescriptor);
    auto* externalOutputBuffer =
        NLS::Render::FrameGraph::ResolveExternalSceneOutputFramebuffer(m_frameDescriptor);
    const bool usesExplicitRecording =
        Context::DriverRendererAccess::HasExplicitRHI(m_driver) &&
        GetActiveExplicitCommandBuffer() != nullptr;
    if (usesExplicitRecording && externalOutputBuffer != nullptr)
    {
        NLS::Render::FrameGraph::TransitionExternalSceneOutputToShaderRead(
            m_driver,
            m_frameDescriptor);
    }
    bool threadedFramePublished = false;
    if (usesExplicitRecording)
    {
        Context::RenderThreadCoordinator::EndRendererFrame(m_driver, shouldPresentSwapchain);
    }
    else if (usesThreadedRendering)
    {
        threadedFramePublished = TryPublishThreadedFrame();
    }

    if (usesThreadedRendering && !threadedFramePublished)
    {
        OnThreadedFramePublishFailed();
    }

    m_isDrawing = false;
    m_frameActive = false;
    const bool releaseGlobalFrame = !m_globalFrameReleaseDeferred;
    m_globalFrameReleaseDeferred = false;
    if (releaseGlobalFrame)
        s_isDrawing.store(false);
}

bool ABaseRenderer::TryBeginGlobalFrameForBackgroundPreview()
{
    bool expectedDrawing = false;
    if (!s_isDrawing.compare_exchange_strong(expectedDrawing, true))
        return false;
    return true;
}

void ABaseRenderer::EndGlobalFrameForBackgroundPreview()
{
    s_isDrawing.store(false);
}

const Data::FrameDescriptor& ABaseRenderer::GetFrameDescriptor() const
{
    NLS_ASSERT(m_isDrawing, "Cannot call GetFrameDescriptor() outside of a frame");
    return m_frameDescriptor;
}

Context::Driver& ABaseRenderer::GetDriver()
{
    return m_driver;
}

const Context::Driver& ABaseRenderer::GetDriver() const
{
    return m_driver;
}

ABaseRenderer::PipelineState ABaseRenderer::CreatePipelineState() const
{
    return m_basePipelineState;
}

bool ABaseRenderer::IsDrawing() const
{
	return m_isDrawing;
}

bool ABaseRenderer::IsFrameActive() const
{
    return m_frameActive;
}

bool ABaseRenderer::TryPublishThreadedFrame()
{
    NLS_PROFILE_SCOPE();
    const auto snapshot =
        m_pendingFrameSnapshot.has_value()
            ? m_pendingFrameSnapshot
            : BuildFrameSnapshot(m_frameDescriptor);
    if (snapshot.has_value())
    {
        auto renderSceneBuilder = BuildPreparedRenderSceneBuilder(snapshot.value());
        if (!renderSceneBuilder)
            return false;

        uint64_t publishedFrameId = 0u;
        const bool published = Context::DriverRendererAccess::TryPublishPreparedFrameBuilder(
            m_driver,
            snapshot.value(),
            std::move(renderSceneBuilder),
            nullptr,
            &publishedFrameId,
            m_globalFrameReleaseDeferred);
        if (published)
            OnThreadedFramePublished(publishedFrameId);
        return published;
    }

    return false;
}

void ABaseRenderer::OnThreadedFramePublished(uint64_t)
{
    m_lastThreadedFramePublished = true;
    m_nextFramePostSubmitTextureReadbacks.clear();
}

void ABaseRenderer::OnThreadedFramePublishFailed()
{
    m_lastThreadedFramePublished = false;
    m_nextFramePostSubmitTextureReadbacks.clear();
}

Context::PreparedRenderSceneBuilder ABaseRenderer::BuildPreparedRenderSceneBuilder(
    const Context::FrameSnapshot& snapshot) const
{
    if (m_pendingPreparedRenderSceneBuilder)
        return m_pendingPreparedRenderSceneBuilder;

    return [snapshot]() mutable
    {
        return Context::BuildSnapshotOwnedRenderScenePackage(snapshot);
    };
}

void ABaseRenderer::SetPendingFrameSnapshot(Context::FrameSnapshot snapshot)
{
    m_pendingFrameSnapshot = std::move(snapshot);
}

void ABaseRenderer::SetPendingPreparedRenderSceneBuilder(
    Context::PreparedRenderSceneBuilder renderSceneBuilder)
{
    m_pendingPreparedRenderSceneBuilder = std::move(renderSceneBuilder);
}

std::optional<NLS::Render::Context::FrameSnapshot> ABaseRenderer::BuildFrameSnapshot(
    const Data::FrameDescriptor& frameDescriptor) const
{
    if (!frameDescriptor.IsValid())
        return std::nullopt;

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.renderWidth = frameDescriptor.renderWidth;
    snapshot.renderHeight = frameDescriptor.renderHeight;
    const auto externalOutputSummary =
        NLS::Render::FrameGraph::BuildExternalSceneOutputSummary(frameDescriptor);
    snapshot.targetsSwapchain = externalOutputSummary.targetsSwapchain;
    snapshot.hasExternalOutput = externalOutputSummary.hasExternalOutput;
    snapshot.externalOutputIdentity = externalOutputSummary.identity;
    snapshot.externalOutputIdentities = externalOutputSummary.identities;
    snapshot.externalOutputTextureCount = externalOutputSummary.textureCount;
    if (externalOutputSummary.hasExternalOutput)
    {
        snapshot.externalOutputColorView =
            NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(frameDescriptor).outputColorView;
    }
    const auto& camera = *frameDescriptor.camera;
    snapshot.clearColor = ResolveFrameClearColor(frameDescriptor);
    snapshot.clearColorBuffer = camera.GetClearColorBuffer();
    snapshot.clearDepthBuffer = camera.GetClearDepthBuffer();
    snapshot.clearStencilBuffer = camera.GetClearStencilBuffer();
    snapshot.hasGeometryFrustum = camera.GetGeometryFrustum() != nullptr;
    snapshot.hasLightFrustum = camera.GetLightFrustum() != nullptr;
    snapshot.recordedDrawCommands = m_threadedRecordedDrawCommands;
    snapshot.postSubmitTextureReadbacks = m_nextFramePostSubmitTextureReadbacks;
    return snapshot;
}

void ABaseRenderer::SetNextFramePostSubmitTextureReadback(
    Context::PostSubmitTextureReadbackRequest request)
{
    m_nextFramePostSubmitTextureReadbacks.clear();
    m_nextFramePostSubmitTextureReadbacks.push_back(std::move(request));
}

bool ABaseRenderer::WasLastThreadedFramePublished() const
{
    return m_lastThreadedFramePublished;
}

NLS::Render::FrameGraph::FrameGraphExecutionContext ABaseRenderer::CreateFrameGraphExecutionContext() const
{
    return Context::DriverRendererAccess::CreateFrameGraphExecutionContext(m_driver);
}

void ABaseRenderer::SetActivePreparedPassBindingSet(const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
{
    m_activePreparedPassBindingSet = bindingSet;
}

bool ABaseRenderer::BeginSingleTargetRenderPass(
    Buffers::Framebuffer* p_framebuffer,
    uint16_t p_width,
    uint16_t p_height,
    bool p_clearColor,
    bool p_clearDepth,
    bool p_clearStencil,
    const std::shared_ptr<RHI::RHITextureView>& p_depthStencilViewOverride,
    bool p_writesDepthStencilAttachment,
    const Maths::Vector4& p_clearValue)
{
    NLS_PROFILE_SCOPE();

    auto commandBuffer = GetActiveExplicitCommandBuffer();
    if (commandBuffer == nullptr)
        return false;

    NLS_ASSERT(!m_recordedRenderPassActive, "Cannot begin a recorded render pass while another one is active.");

    NLS::Render::RHI::RHIRenderPassDesc renderPassDesc;
    renderPassDesc.renderArea = { 0, 0, p_width, p_height };
    renderPassDesc.debugName = p_framebuffer != nullptr ? "FramebufferRenderPass" : "BackbufferRenderPass";
    const bool useResourceStateTracker =
        Context::DriverRendererAccess::CreateFrameGraphExecutionContext(m_driver).CanTrackExplicitResourceState();
    renderPassDesc.attachmentsRequireExternalStateTransitions = useResourceStateTracker;

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
        auto swapchainView = Context::DriverRendererAccess::GetSwapchainBackbufferView(m_driver);
        if (swapchainView == nullptr)
            return false;

        colorAttachment.view = swapchainView;
        renderPassDesc.colorAttachments.push_back(std::move(colorAttachment));
    }

    auto depthStencilView = p_depthStencilViewOverride;
    const bool hasDepthStencilViewOverride = depthStencilView != nullptr;
    if (!hasDepthStencilViewOverride &&
        p_framebuffer != nullptr &&
        (p_clearDepth || p_framebuffer->GetExplicitDepthStencilTextureHandle() != nullptr))
    {
        depthStencilView = p_framebuffer->GetOrCreateExplicitDepthStencilView("FramebufferDepthView");
    }
    else if (!hasDepthStencilViewOverride && p_framebuffer == nullptr)
    {
        depthStencilView = Context::DriverRendererAccess::GetSwapchainDepthStencilView(m_driver);
    }

    if (depthStencilView != nullptr)
    {
        const bool writesDepthStencil = p_writesDepthStencilAttachment || p_clearDepth || p_clearStencil;
        const bool preserveFramebufferStencil =
            !hasDepthStencilViewOverride && p_framebuffer != nullptr;
        NLS::Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthAttachment;
        depthAttachment.depthLoadOp = p_clearDepth ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
        depthAttachment.depthStoreOp = NLS::Render::RHI::StoreOp::Store;
        depthAttachment.stencilLoadOp = p_clearStencil
            ? NLS::Render::RHI::LoadOp::Clear
            : (preserveFramebufferStencil ? NLS::Render::RHI::LoadOp::Load : NLS::Render::RHI::LoadOp::DontCare);
        depthAttachment.stencilStoreOp = NLS::Render::RHI::StoreOp::Store;
        depthAttachment.clearValue.depth = 1.0f;
        depthAttachment.clearValue.stencil = 0u;
        depthAttachment.view = std::move(depthStencilView);
        depthAttachment.readOnlyDepthStencil = !writesDepthStencil;
        renderPassDesc.depthStencilAttachment = std::move(depthAttachment);
    }

    if (useResourceStateTracker)
    {
        NLS::Render::RHI::RHIBarrierDesc attachmentBarriers;
        attachmentBarriers.textureBarriers.reserve(
            renderPassDesc.colorAttachments.size() +
            (renderPassDesc.depthStencilAttachment.has_value() ? 1u : 0u));
        for (const auto& colorAttachment : renderPassDesc.colorAttachments)
        {
            if (colorAttachment.view == nullptr || colorAttachment.view->GetTexture() == nullptr)
                continue;

            attachmentBarriers.textureBarriers.push_back({
                colorAttachment.view->GetTexture(),
                NLS::Render::RHI::ResourceState::Unknown,
                NLS::Render::RHI::ResourceState::RenderTarget,
                colorAttachment.view->GetDesc().subresourceRange,
                NLS::Render::RHI::PipelineStageMask::AllCommands,
                NLS::Render::RHI::PipelineStageMask::RenderTarget,
                NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
                NLS::Render::RHI::AccessMask::ColorAttachmentRead | NLS::Render::RHI::AccessMask::ColorAttachmentWrite
            });
        }
        if (renderPassDesc.depthStencilAttachment.has_value() &&
            renderPassDesc.depthStencilAttachment->view != nullptr &&
            renderPassDesc.depthStencilAttachment->view->GetTexture() != nullptr)
        {
            const bool writesDepthStencil = !renderPassDesc.depthStencilAttachment->readOnlyDepthStencil;
            attachmentBarriers.textureBarriers.push_back({
                renderPassDesc.depthStencilAttachment->view->GetTexture(),
                NLS::Render::RHI::ResourceState::Unknown,
                writesDepthStencil
                    ? NLS::Render::RHI::ResourceState::DepthWrite
                    : NLS::Render::RHI::ResourceState::DepthRead,
                renderPassDesc.depthStencilAttachment->view->GetDesc().subresourceRange,
                NLS::Render::RHI::PipelineStageMask::AllCommands,
                NLS::Render::RHI::PipelineStageMask::DepthStencil,
                NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
                writesDepthStencil
                    ? (NLS::Render::RHI::AccessMask::DepthStencilRead |
                        NLS::Render::RHI::AccessMask::DepthStencilWrite)
                    : NLS::Render::RHI::AccessMask::DepthStencilRead
            });
        }
        if (!attachmentBarriers.textureBarriers.empty())
        {
            auto executionContext = CreateFrameGraphExecutionContext();
            executionContext.RecordResourceBarriers(attachmentBarriers);
        }
    }

    CaptureRecordedPassAttachmentViews(renderPassDesc);
    commandBuffer->BeginRenderPass(renderPassDesc);
    commandBuffer->BeginGpuProfileScope(renderPassDesc.debugName, __FUNCTION__);
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
    return BeginRecordedRenderPass(
        p_framebuffer,
        p_width,
        p_height,
        p_clearColor,
        p_clearDepth,
        p_clearStencil,
        true,
        p_clearValue);
}

bool ABaseRenderer::BeginRecordedRenderPass(
    Buffers::MultiFramebuffer* p_framebuffer,
    uint16_t p_width,
    uint16_t p_height,
    bool p_clearColor,
    bool p_clearDepth,
    bool p_clearStencil,
    bool p_writesDepthStencilAttachment,
    const Maths::Vector4& p_clearValue)
{
    NLS_PROFILE_SCOPE();

    auto commandBuffer = GetActiveExplicitCommandBuffer();
    if (commandBuffer == nullptr || p_framebuffer == nullptr)
        return false;

    NLS_ASSERT(!m_recordedRenderPassActive, "Cannot begin a recorded render pass while another one is active.");

    NLS::Render::RHI::RHIRenderPassDesc renderPassDesc;
    renderPassDesc.renderArea = { 0, 0, p_width, p_height };
    renderPassDesc.debugName = "MultiFramebufferRenderPass";
    const bool useResourceStateTracker =
        Context::DriverRendererAccess::CreateFrameGraphExecutionContext(m_driver).CanTrackExplicitResourceState();
    renderPassDesc.attachmentsRequireExternalStateTransitions = useResourceStateTracker;

    const auto& colorTextures = p_framebuffer->GetExplicitColorTextureHandles();
    for (size_t i = 0; i < colorTextures.size(); ++i)
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
        const bool writesDepthStencil = p_writesDepthStencilAttachment || p_clearDepth || p_clearStencil;
        NLS::Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthAttachment;
        depthAttachment.depthLoadOp = p_clearDepth ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
        depthAttachment.depthStoreOp = NLS::Render::RHI::StoreOp::Store;
        depthAttachment.stencilLoadOp = p_clearStencil ? NLS::Render::RHI::LoadOp::Clear : NLS::Render::RHI::LoadOp::Load;
        depthAttachment.stencilStoreOp = NLS::Render::RHI::StoreOp::Store;
        depthAttachment.clearValue.depth = 1.0f;
        depthAttachment.clearValue.stencil = 0u;
        depthAttachment.view = p_framebuffer->GetOrCreateExplicitDepthView("MultiFramebufferDepthView");
        depthAttachment.readOnlyDepthStencil = !writesDepthStencil;
        renderPassDesc.depthStencilAttachment = std::move(depthAttachment);
    }

    if (useResourceStateTracker)
    {
        NLS::Render::RHI::RHIBarrierDesc attachmentBarriers;
        attachmentBarriers.textureBarriers.reserve(
            renderPassDesc.colorAttachments.size() +
            (renderPassDesc.depthStencilAttachment.has_value() ? 1u : 0u));
        for (const auto& colorAttachment : renderPassDesc.colorAttachments)
        {
            if (colorAttachment.view == nullptr || colorAttachment.view->GetTexture() == nullptr)
                continue;

            attachmentBarriers.textureBarriers.push_back({
                colorAttachment.view->GetTexture(),
                NLS::Render::RHI::ResourceState::Unknown,
                NLS::Render::RHI::ResourceState::RenderTarget,
                colorAttachment.view->GetDesc().subresourceRange,
                NLS::Render::RHI::PipelineStageMask::AllCommands,
                NLS::Render::RHI::PipelineStageMask::RenderTarget,
                NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
                NLS::Render::RHI::AccessMask::ColorAttachmentRead | NLS::Render::RHI::AccessMask::ColorAttachmentWrite
            });
        }
        if (renderPassDesc.depthStencilAttachment.has_value() &&
            renderPassDesc.depthStencilAttachment->view != nullptr &&
            renderPassDesc.depthStencilAttachment->view->GetTexture() != nullptr)
        {
            const bool writesDepthStencil = !renderPassDesc.depthStencilAttachment->readOnlyDepthStencil;
            attachmentBarriers.textureBarriers.push_back({
                renderPassDesc.depthStencilAttachment->view->GetTexture(),
                NLS::Render::RHI::ResourceState::Unknown,
                writesDepthStencil
                    ? NLS::Render::RHI::ResourceState::DepthWrite
                    : NLS::Render::RHI::ResourceState::DepthRead,
                renderPassDesc.depthStencilAttachment->view->GetDesc().subresourceRange,
                NLS::Render::RHI::PipelineStageMask::AllCommands,
                NLS::Render::RHI::PipelineStageMask::DepthStencil,
                NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
                writesDepthStencil
                    ? (NLS::Render::RHI::AccessMask::DepthStencilRead |
                        NLS::Render::RHI::AccessMask::DepthStencilWrite)
                    : NLS::Render::RHI::AccessMask::DepthStencilRead
            });
        }
        if (!attachmentBarriers.textureBarriers.empty())
        {
            auto executionContext = CreateFrameGraphExecutionContext();
            executionContext.RecordResourceBarriers(attachmentBarriers);
        }
    }

    CaptureRecordedPassAttachmentViews(renderPassDesc);
    commandBuffer->BeginRenderPass(renderPassDesc);
    commandBuffer->BeginGpuProfileScope(renderPassDesc.debugName, __FUNCTION__);
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
    const Maths::Vector4& p_clearValue)
{
    NLS_PROFILE_SCOPE();

    return BeginRecordedRenderPass(
        NLS::Render::FrameGraph::ResolveExternalSceneOutputFramebuffer(m_frameDescriptor),
        p_width,
        p_height,
        p_clearColor,
        p_clearDepth,
        p_clearStencil,
        p_clearValue);
}

bool ABaseRenderer::BeginOutputRenderPass(
    uint16_t p_width,
    uint16_t p_height,
    bool p_clearColor,
    bool p_clearDepth,
    bool p_clearStencil,
    const std::shared_ptr<RHI::RHITextureView>& p_depthStencilView,
    bool p_writesDepthStencilAttachment,
    const Maths::Vector4& p_clearValue)
{
    NLS_PROFILE_SCOPE();

    return BeginSingleTargetRenderPass(
        NLS::Render::FrameGraph::ResolveExternalSceneOutputFramebuffer(m_frameDescriptor),
        p_width,
        p_height,
        p_clearColor,
        p_clearDepth,
        p_clearStencil,
        p_depthStencilView,
        p_writesDepthStencilAttachment,
        p_clearValue);
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
    NLS_PROFILE_SCOPE();

    return BeginSingleTargetRenderPass(
        p_framebuffer,
        p_width,
        p_height,
        p_clearColor,
        p_clearDepth,
        p_clearStencil,
        {},
        true,
        p_clearValue);
}

void ABaseRenderer::EndRecordedRenderPass()
{
    NLS_PROFILE_SCOPE();

    auto commandBuffer = GetActiveExplicitCommandBuffer();
    if (commandBuffer == nullptr || !m_recordedRenderPassActive)
        return;

    const auto& colorViews = m_activeRecordedPassColorViews;
    const auto depthStencilView = m_activeRecordedPassDepthStencilView;

    commandBuffer->EndGpuProfileScope();
    commandBuffer->EndRenderPass();

    if (!colorViews.empty() || depthStencilView != nullptr)
    {
        auto executionContext = CreateFrameGraphExecutionContext();
        if (executionContext.CanTrackExplicitResourceState())
        {
            NLS::Render::RHI::RHIBarrierDesc attachmentBarriers;
            for (const auto& colorView : colorViews)
            {
                const auto transition =
                    NLS::Render::FrameGraph::BuildSampledAttachmentEndTransition(colorView, false);
                if (transition.has_value())
                    AppendTextureVisibilityTransitionBarrier(attachmentBarriers, *transition);
            }

            const auto depthTransition =
                NLS::Render::FrameGraph::BuildSampledAttachmentEndTransition(depthStencilView, true);
            if (depthTransition.has_value())
                AppendTextureVisibilityTransitionBarrier(attachmentBarriers, *depthTransition);

            if (!attachmentBarriers.textureBarriers.empty())
                executionContext.RecordResourceBarriers(attachmentBarriers);
        }
    }

    m_recordedRenderPassActive = false;
    m_activeRecordedPassColorViews.clear();
    m_activeRecordedPassDepthStencilView.reset();
}

void ABaseRenderer::EndOutputRenderPass(const bool p_startedRecordedPass)
{
    NLS_PROFILE_SCOPE();

    if (p_startedRecordedPass)
        EndRecordedRenderPass();
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

bool ABaseRenderer::HasActiveReadbackSource() const
{
    return Context::DriverRendererAccess::ResolveReadbackTexture(m_driver) != nullptr;
}

std::shared_ptr<NLS::Render::RHI::RHIBindingSet> ABaseRenderer::CreateExplicitUniformBufferBindingSet(
    const NLS::Render::Buffers::UniformBuffer& buffer,
    const ExplicitUniformBufferBindingDesc& desc) const
{
    if (!m_frameActive)
        InvalidateExplicitDeviceDependentCachesIfNeeded();
    const auto& device = m_preparedRecordedDrawDevice;
    const auto cacheKey = BuildExplicitUniformBindingLayoutCacheKey(desc, device);

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
            desc.registerSpace,
            0u
        });
        layout = Context::DriverRendererAccess::CreateExplicitBindingLayout(m_driver, layoutDesc);
        if (layout != nullptr)
            m_explicitUniformBindingLayouts[cacheKey] = layout;
    }

    if (layout == nullptr)
        return nullptr;

    const auto snapshotBuffer = buffer.CreateExplicitSnapshotBuffer(desc.snapshotDebugName);
    if (snapshotBuffer != nullptr)
        ++m_explicitUniformSnapshotBufferCreationCount;
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
        0u,
        nullptr,
        nullptr
    });
    const auto allocationLifetime = Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver)
        ? NLS::Render::RHI::DescriptorAllocationLifetime::Persistent
        : NLS::Render::RHI::DescriptorAllocationLifetime::TransientFrame;
    auto bindingSet = Context::DriverRendererAccess::CreateExplicitBindingSet(m_driver,
        bindingSetDesc,
        allocationLifetime);
    if (bindingSet != nullptr)
        ++m_explicitUniformBindingSetCreationCount;
    return bindingSet;
}

bool ABaseRenderer::IsRenderCoordinateInBounds(uint32_t p_x, uint32_t p_y) const
{
    return p_x < m_frameDescriptor.renderWidth && p_y < m_frameDescriptor.renderHeight;
}

uint64_t ABaseRenderer::GetExplicitUniformBindingSetCreationCount() const
{
    return m_explicitUniformBindingSetCreationCount;
}

uint64_t ABaseRenderer::GetExplicitUniformSnapshotBufferCreationCount() const
{
    return m_explicitUniformSnapshotBufferCreationCount;
}

void ABaseRenderer::CaptureRecordedPassAttachmentViews(const NLS::Render::RHI::RHIRenderPassDesc& renderPassDesc)
{
    m_activeRecordedPassColorViews.clear();
    m_activeRecordedPassColorViews.reserve(renderPassDesc.colorAttachments.size());
    for (const auto& colorAttachment : renderPassDesc.colorAttachments)
    {
        if (colorAttachment.view != nullptr)
            m_activeRecordedPassColorViews.push_back(colorAttachment.view);
    }

    m_activeRecordedPassDepthStencilView = renderPassDesc.depthStencilAttachment.has_value()
        ? renderPassDesc.depthStencilAttachment->view
        : nullptr;
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
    const auto result = ReadPixelsChecked(
        p_x,
        p_y,
        p_width,
        p_height,
        p_format,
        p_type,
        p_data);
    if (!result.Succeeded())
        NLS_LOG_WARNING("[ABaseRenderer] ReadPixels failed: " + result.message);
}

NLS::Render::RHI::RHIReadbackResult ABaseRenderer::ReadPixelsChecked(
    uint32_t p_x,
    uint32_t p_y,
    uint32_t p_width,
    uint32_t p_height,
    Settings::EPixelDataFormat p_format,
    Settings::EPixelDataType p_type,
    void* p_data
) const
{
    return Context::DriverRendererAccess::ReadPixelsChecked(
        m_driver,
        p_x,
        p_y,
        p_width,
        p_height,
        p_format,
        p_type,
        p_data);
}

NLS::Render::RHI::RHIReadbackResult ABaseRenderer::BeginReadPixels(
    uint32_t p_x,
    uint32_t p_y,
    uint32_t p_width,
    uint32_t p_height,
    Settings::EPixelDataFormat p_format,
    Settings::EPixelDataType p_type,
    void* p_data
) const
{
    return Context::DriverRendererAccess::BeginReadPixels(
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
    return PrepareRecordedDraw(p_pso, p_drawable, "Forward", outDraw);
}

bool ABaseRenderer::PrepareRecordedDraw(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable,
    std::string_view lightMode,
    PreparedRecordedDraw& outDraw) const
{
    auto material = p_drawable.material;
    if (material == nullptr)
    {
        LogRecordedDrawPreparationState(m_driver, "pso", "material_null", p_drawable);
        return false;
    }

    const auto pipelineOverrides = BuildMaterialPipelineStateOverrides(p_pso, *material, m_frameDescriptor);
    if (!pipelineOverrides.has_value())
    {
        LogRecordedDrawPreparationState(m_driver, "pso", "attachment_sample_count_mismatch", p_drawable);
        return false;
    }

    auto mesh = p_drawable.mesh;
    if (mesh == nullptr)
    {
        LogRecordedDrawPreparationState(m_driver, "pso", "mesh_null", p_drawable);
        return false;
    }

    const auto gpuInstances = material->GetGPUInstances();
    if (gpuInstances <= 0)
    {
        LogRecordedDrawPreparationState(m_driver, "pso", "gpu_instances_zero", p_drawable);
        return false;
    }

    auto commandBuffer = GetActiveExplicitCommandBuffer();
    const auto* staticBase = ResolvePreparedRecordedDrawStaticBase(
        "pso",
        p_drawable,
        *material,
        *pipelineOverrides,
        Settings::EComparaisonAlgorithm::LESS,
        p_pso,
        lightMode);
    if (staticBase == nullptr)
        return false;

    PopulatePreparedRecordedDrawFromStaticBase(outDraw, std::move(commandBuffer), p_drawable, *staticBase);
    return true;
}

bool ABaseRenderer::PreparedRecordedDrawStaticBaseCacheKey::operator==(
    const PreparedRecordedDrawStaticBaseCacheKey& rhs) const
{
    return deviceIdentity == rhs.deviceIdentity &&
        backend == rhs.backend &&
        materialInstanceId == rhs.materialInstanceId &&
        materialParameterRevision == rhs.materialParameterRevision &&
        materialRenderStateRevision == rhs.materialRenderStateRevision &&
        materialBindingRevision == rhs.materialBindingRevision &&
        materialShaderInstanceId == rhs.materialShaderInstanceId &&
        materialShaderGeneration == rhs.materialShaderGeneration &&
        meshInstanceId == rhs.meshInstanceId &&
        meshContentRevision == rhs.meshContentRevision &&
        passBindingSetAddress == rhs.passBindingSetAddress &&
        primitiveMode == rhs.primitiveMode &&
        depthCompareOverride == rhs.depthCompareOverride &&
        pipelineState.bits == rhs.pipelineState.bits &&
        pipelineOverrides == rhs.pipelineOverrides;
}

size_t ABaseRenderer::PreparedRecordedDrawStaticBaseCacheKeyHash::operator()(
    const PreparedRecordedDrawStaticBaseCacheKey& key) const
{
    size_t seed = 0u;
    HashCombine(seed, key.deviceIdentity);
    HashCombine(seed, static_cast<uint32_t>(key.backend));
    HashCombine(seed, key.materialInstanceId);
    HashCombine(seed, key.materialParameterRevision);
    HashCombine(seed, key.materialRenderStateRevision);
    HashCombine(seed, key.materialBindingRevision);
    HashCombine(seed, key.materialShaderInstanceId);
    HashCombine(seed, key.materialShaderGeneration);
    HashCombine(seed, key.meshInstanceId);
    HashCombine(seed, key.meshContentRevision);
    HashCombine(seed, key.passBindingSetAddress);
    HashCombine(seed, static_cast<uint32_t>(key.primitiveMode));
    HashCombine(seed, static_cast<uint32_t>(key.depthCompareOverride));
    HashCombine(seed, HashPipelineState(key.pipelineState));
    HashCombine(seed, key.pipelineOverrides.GetHash());
    return seed;
}

bool ABaseRenderer::PreparedRecordedDrawStaticBaseStableKey::operator==(
    const PreparedRecordedDrawStaticBaseStableKey& rhs) const
{
    return deviceIdentity == rhs.deviceIdentity &&
        backend == rhs.backend &&
        materialInstanceId == rhs.materialInstanceId &&
        materialShaderInstanceId == rhs.materialShaderInstanceId &&
        meshInstanceId == rhs.meshInstanceId &&
        passBindingSetAddress == rhs.passBindingSetAddress &&
        primitiveMode == rhs.primitiveMode &&
        depthCompareOverride == rhs.depthCompareOverride &&
        pipelineState.bits == rhs.pipelineState.bits &&
        pipelineOverrides == rhs.pipelineOverrides;
}

size_t ABaseRenderer::PreparedRecordedDrawStaticBaseStableKeyHash::operator()(
    const PreparedRecordedDrawStaticBaseStableKey& key) const
{
    size_t seed = 0u;
    HashCombine(seed, key.deviceIdentity);
    HashCombine(seed, static_cast<uint32_t>(key.backend));
    HashCombine(seed, key.materialInstanceId);
    HashCombine(seed, key.materialShaderInstanceId);
    HashCombine(seed, key.meshInstanceId);
    HashCombine(seed, key.passBindingSetAddress);
    HashCombine(seed, static_cast<uint32_t>(key.primitiveMode));
    HashCombine(seed, static_cast<uint32_t>(key.depthCompareOverride));
    HashCombine(seed, HashPipelineState(key.pipelineState));
    HashCombine(seed, key.pipelineOverrides.GetHash());
    return seed;
}

ABaseRenderer::PreparedRecordedDrawStaticBaseStableKey ABaseRenderer::BuildPreparedRecordedDrawStaticBaseStableKey(
    const PreparedRecordedDrawStaticBaseCacheKey& key)
{
    PreparedRecordedDrawStaticBaseStableKey stableKey;
    stableKey.deviceIdentity = key.deviceIdentity;
    stableKey.backend = key.backend;
    stableKey.materialInstanceId = key.materialInstanceId;
    stableKey.materialShaderInstanceId = key.materialShaderInstanceId;
    stableKey.meshInstanceId = key.meshInstanceId;
    stableKey.passBindingSetAddress = key.passBindingSetAddress;
    stableKey.primitiveMode = key.primitiveMode;
    stableKey.depthCompareOverride = key.depthCompareOverride;
    stableKey.pipelineState = key.pipelineState;
    stableKey.pipelineOverrides = key.pipelineOverrides;
    return stableKey;
}

ABaseRenderer::PreparedRecordedDrawStaticBaseCacheKey ABaseRenderer::BuildPreparedRecordedDrawStaticBaseCacheKey(
    const Entities::Drawable& drawable,
    const Resources::Material& material,
    const std::shared_ptr<RHI::RHIDevice>& device,
    const Resources::MaterialPipelineStateOverrides& pipelineOverrides,
    const Settings::EComparaisonAlgorithm depthCompareOverride,
    const Data::PipelineState& pipelineState,
    const std::shared_ptr<RHI::RHIBindingSet>& passBindingSet,
    const Resources::Shader* effectiveShader)
{
    PreparedRecordedDrawStaticBaseCacheKey key;
    key.deviceIdentity = device != nullptr ? device->GetCacheIdentity() : 0u;
    key.backend = ResolveDeviceBackendType(device);
    key.materialInstanceId = material.GetInstanceId();
    key.materialParameterRevision = material.GetParameterRevision();
    key.materialRenderStateRevision = material.GetRenderStateRevision();
    key.materialBindingRevision = material.GetBindingRevision();
    const auto* shader = effectiveShader != nullptr ? effectiveShader : material.GetShader();
    key.materialShaderInstanceId = shader != nullptr ? shader->GetInstanceId() : 0u;
    key.materialShaderGeneration = shader != nullptr ? shader->GetGeneration() : 0u;
    key.meshInstanceId = drawable.mesh != nullptr ? drawable.mesh->GetInstanceId() : 0u;
    key.meshContentRevision = drawable.mesh != nullptr ? drawable.mesh->GetContentRevision() : 0u;
    key.passBindingSetAddress = reinterpret_cast<uintptr_t>(passBindingSet.get());
    key.primitiveMode = drawable.primitiveMode;
    key.depthCompareOverride = depthCompareOverride;
    key.pipelineState = pipelineState;
    key.pipelineOverrides = pipelineOverrides;
    return key;
}

void ABaseRenderer::PopulatePreparedRecordedDrawFromStaticBase(
    PreparedRecordedDraw& outDraw,
    std::shared_ptr<RHI::RHICommandBuffer> commandBuffer,
    const Entities::Drawable& drawable,
    const PreparedRecordedDrawStaticBase& staticBase)
{
    outDraw.commandBuffer = std::move(commandBuffer);
    outDraw.pipeline = staticBase.pipeline;
    outDraw.materialBindingSet = staticBase.materialBindingSet;
    outDraw.passBindingSet = staticBase.passBindingSet;
    outDraw.mesh = staticBase.mesh;
    outDraw.instanceCount = drawable.instanceCount != 0u
        ? drawable.instanceCount
        : static_cast<uint32_t>(staticBase.gpuInstances);
    outDraw.vertexStart = drawable.vertexStart;
    outDraw.vertexCount = drawable.vertexCount;
}

void ABaseRenderer::EraseStalePreparedRecordedDrawStaticBaseEntries(
    const PreparedRecordedDrawStaticBaseCacheKey& currentKey) const
{
    const auto stableKey = BuildPreparedRecordedDrawStaticBaseStableKey(currentKey);
    const auto indexed = m_preparedRecordedDrawStaticBaseStableIndex.find(stableKey);
    if (indexed == m_preparedRecordedDrawStaticBaseStableIndex.end() || indexed->second == currentKey)
        return;

    ErasePreparedRecordedDrawStaticBaseEntry(indexed->second);
}

void ABaseRenderer::ErasePreparedRecordedDrawStaticBaseEntry(
    const PreparedRecordedDrawStaticBaseCacheKey& key) const
{
    if (const auto found = m_preparedRecordedDrawStaticBaseCache.find(key);
        found != m_preparedRecordedDrawStaticBaseCache.end())
    {
        if (found->second.lruLinked)
            m_preparedRecordedDrawStaticBaseLruKeys.erase(found->second.lruIterator);

        m_preparedRecordedDrawStaticBaseCache.erase(found);
    }
    else
    {
        const auto stale = std::find(
            m_preparedRecordedDrawStaticBaseLruKeys.begin(),
            m_preparedRecordedDrawStaticBaseLruKeys.end(),
            key);
        if (stale != m_preparedRecordedDrawStaticBaseLruKeys.end())
            m_preparedRecordedDrawStaticBaseLruKeys.erase(stale);
    }

    const auto stableKey = BuildPreparedRecordedDrawStaticBaseStableKey(key);
    const auto indexed = m_preparedRecordedDrawStaticBaseStableIndex.find(stableKey);
    if (indexed != m_preparedRecordedDrawStaticBaseStableIndex.end() && indexed->second == key)
        m_preparedRecordedDrawStaticBaseStableIndex.erase(indexed);
}

void ABaseRenderer::IndexPreparedRecordedDrawStaticBaseEntry(
    const PreparedRecordedDrawStaticBaseCacheKey& key) const
{
    m_preparedRecordedDrawStaticBaseStableIndex[BuildPreparedRecordedDrawStaticBaseStableKey(key)] = key;
}

void ABaseRenderer::LinkPreparedRecordedDrawStaticBaseEntry(
    const PreparedRecordedDrawStaticBaseCacheKey& key,
    PreparedRecordedDrawStaticBase& entry) const
{
    entry.lastUsedFrame = m_preparedRecordedDrawStaticBaseCacheFrame;
    m_preparedRecordedDrawStaticBaseLruKeys.push_back(key);
    entry.lruIterator = std::prev(m_preparedRecordedDrawStaticBaseLruKeys.end());
    entry.lruLinked = true;
}

void ABaseRenderer::TouchPreparedRecordedDrawStaticBaseEntry(
    PreparedRecordedDrawStaticBase& entry) const
{
    entry.lastUsedFrame = m_preparedRecordedDrawStaticBaseCacheFrame;
    if (entry.lruLinked)
    {
        m_preparedRecordedDrawStaticBaseLruKeys.splice(
            m_preparedRecordedDrawStaticBaseLruKeys.end(),
            m_preparedRecordedDrawStaticBaseLruKeys,
            entry.lruIterator);
    }
}

void ABaseRenderer::TrimPreparedRecordedDrawStaticBaseCache(const bool includeFrameAgeSweep) const
{
    if (m_preparedRecordedDrawStaticBaseCache.empty())
    {
        m_preparedRecordedDrawStaticBaseStableIndex.clear();
        m_preparedRecordedDrawStaticBaseLruKeys.clear();
        return;
    }

    if (includeFrameAgeSweep)
    {
        size_t processedEntryCount = 0u;
        while (!m_preparedRecordedDrawStaticBaseLruKeys.empty() &&
               processedEntryCount < kPreparedRecordedDrawStaticBaseCacheAgeSweepBudget)
        {
            const auto& oldestKey = m_preparedRecordedDrawStaticBaseLruKeys.front();
            const auto oldest = m_preparedRecordedDrawStaticBaseCache.find(oldestKey);
            ++processedEntryCount;
            if (oldest == m_preparedRecordedDrawStaticBaseCache.end())
            {
                m_preparedRecordedDrawStaticBaseLruKeys.pop_front();
                continue;
            }

            const auto age = m_preparedRecordedDrawStaticBaseCacheFrame >= oldest->second.lastUsedFrame
                ? m_preparedRecordedDrawStaticBaseCacheFrame - oldest->second.lastUsedFrame
                : 0u;
            if (age <= kMaxPreparedRecordedDrawStaticBaseCacheFrameAge)
                break;

            const auto expiredKey = oldestKey;
            ErasePreparedRecordedDrawStaticBaseEntry(expiredKey);
        }
    }

    while (m_preparedRecordedDrawStaticBaseCache.size() > kMaxPreparedRecordedDrawStaticBaseCacheEntries &&
           !m_preparedRecordedDrawStaticBaseLruKeys.empty())
    {
        const auto evictedKey = m_preparedRecordedDrawStaticBaseLruKeys.front();
        ErasePreparedRecordedDrawStaticBaseEntry(evictedKey);
    }

    if (m_preparedRecordedDrawStaticBaseCache.empty())
    {
        m_preparedRecordedDrawStaticBaseStableIndex.clear();
        m_preparedRecordedDrawStaticBaseLruKeys.clear();
    }
}

void ABaseRenderer::InvalidateExplicitDeviceDependentCachesIfNeeded() const
{
    const auto device = GetExplicitDevice();
    const auto deviceIdentity = device != nullptr ? device->GetCacheIdentity() : 0u;
    const auto backend = ResolveDeviceBackendType(device);
    m_preparedRecordedDrawDevice = device;
    if (deviceIdentity == m_cachedExplicitDeviceIdentity && backend == m_cachedExplicitDeviceBackend)
        return;

    m_cachedExplicitDeviceIdentity = deviceIdentity;
    m_cachedExplicitDeviceBackend = backend;
    ClearPreparedRecordedDrawStaticBaseCache();
    m_explicitUniformBindingLayouts.clear();
}

const ABaseRenderer::PreparedRecordedDrawStaticBase* ABaseRenderer::ResolvePreparedRecordedDrawStaticBase(
    const char* preparationPath,
    const Entities::Drawable& drawable,
    Resources::Material& material,
    const Resources::MaterialPipelineStateOverrides& pipelineOverrides,
    const Settings::EComparaisonAlgorithm depthCompareOverride,
    const Data::PipelineState& pipelineState,
    const std::string_view lightMode) const
{
    auto* mesh = drawable.mesh;
    if (mesh == nullptr)
        return nullptr;

    const auto& device = m_preparedRecordedDrawDevice;
    const auto* effectiveShader = lightMode.empty()
        ? material.GetShader()
        : material.ResolveShaderForLightMode(lightMode);
    if (effectiveShader == nullptr)
    {
        LogRecordedDrawPreparationState(m_driver, preparationPath, "shaderlab_lightmode_missing", drawable);
        return nullptr;
    }
    const auto passBindingSet = material.RequiresPassDescriptorSet(effectiveShader) ? m_activePreparedPassBindingSet : nullptr;
    const auto key = BuildPreparedRecordedDrawStaticBaseCacheKey(
        drawable,
        material,
        device,
        pipelineOverrides,
        depthCompareOverride,
        pipelineState,
        passBindingSet,
        effectiveShader);
    if (const auto found = m_preparedRecordedDrawStaticBaseCache.find(key);
        found != m_preparedRecordedDrawStaticBaseCache.end())
    {
        TouchPreparedRecordedDrawStaticBaseEntry(found->second);
        if (auto* stats = GetMutableRendererStats(); stats != nullptr)
            stats->RecordPreparedRecordedDrawStaticBaseCache(true);
        return &found->second;
    }

    auto bindingSet = material.GetRecordedBindingSet(device, effectiveShader);
    const auto finalKey = BuildPreparedRecordedDrawStaticBaseCacheKey(
        drawable,
        material,
        device,
        pipelineOverrides,
        depthCompareOverride,
        pipelineState,
        passBindingSet,
        effectiveShader);
    if (finalKey != key)
    {
        if (const auto found = m_preparedRecordedDrawStaticBaseCache.find(finalKey);
            found != m_preparedRecordedDrawStaticBaseCache.end())
        {
            TouchPreparedRecordedDrawStaticBaseEntry(found->second);
            if (auto* stats = GetMutableRendererStats(); stats != nullptr)
                stats->RecordPreparedRecordedDrawStaticBaseCache(true);
            return &found->second;
        }
    }

    auto rhiMesh = mesh->GetRHIMesh();
    auto pipelineCache = Context::DriverRendererAccess::GetPipelineCache(m_driver);
    bool hasPipelineLayout = false;
    bool hasVertexShader = false;
    bool hasFragmentShader = false;
    auto pipeline = material.BuildRecordedGraphicsPipeline(
        device,
        pipelineCache,
        drawable.primitiveMode,
        pipelineState,
        pipelineOverrides,
        &hasPipelineLayout,
        &hasVertexShader,
        &hasFragmentShader,
        effectiveShader);

    if (pipeline == nullptr || bindingSet == nullptr || rhiMesh == nullptr)
    {
        LogRecordedDrawPreparationState(
            m_driver,
            preparationPath,
            pipeline == nullptr ? "pipeline_null" :
                bindingSet == nullptr ? "material_binding_set_null" : "rhi_mesh_null",
            drawable,
            device,
            pipelineCache,
            pipeline,
            bindingSet,
            rhiMesh,
            hasPipelineLayout,
            hasVertexShader,
            hasFragmentShader);
        return nullptr;
    }

    PreparedRecordedDrawStaticBase staticBase;
    staticBase.pipeline = std::move(pipeline);
    staticBase.materialBindingSet = std::move(bindingSet);
    staticBase.passBindingSet = passBindingSet;
    staticBase.mesh = std::move(rhiMesh);
    staticBase.gpuInstances = material.GetGPUInstances();
    EraseStalePreparedRecordedDrawStaticBaseEntries(finalKey);
    auto [entry, inserted] = m_preparedRecordedDrawStaticBaseCache.emplace(finalKey, std::move(staticBase));
    if (inserted)
        LinkPreparedRecordedDrawStaticBaseEntry(finalKey, entry->second);
    else
        TouchPreparedRecordedDrawStaticBaseEntry(entry->second);
    IndexPreparedRecordedDrawStaticBaseEntry(finalKey);
    TrimPreparedRecordedDrawStaticBaseCache(false);
    if (auto* stats = GetMutableRendererStats(); stats != nullptr)
        stats->RecordPreparedRecordedDrawStaticBaseCache(false);
    return &entry->second;
}

bool ABaseRenderer::PrepareRecordedDraw(
    const Entities::Drawable& p_drawable,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride,
    PreparedRecordedDraw& outDraw) const
{
    return PrepareRecordedDraw(
        p_drawable,
        std::move(pipelineOverrides),
        depthCompareOverride,
        "Forward",
        outDraw);
}

bool ABaseRenderer::PrepareRecordedDraw(
    const Entities::Drawable& p_drawable,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride,
    std::string_view lightMode,
    PreparedRecordedDraw& outDraw) const
{
    auto* material = p_drawable.material;
    if (material == nullptr)
    {
        LogRecordedDrawPreparationState(m_driver, "overrides", "material_null", p_drawable);
        return false;
    }

    return PrepareRecordedDraw(
        p_drawable,
        *material,
        std::move(pipelineOverrides),
        depthCompareOverride,
        lightMode,
        outDraw);
}

bool ABaseRenderer::PrepareRecordedDraw(
    const Entities::Drawable& p_drawable,
    Resources::Material& effectiveMaterial,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride,
    std::string_view lightMode,
    PreparedRecordedDraw& outDraw) const
{
    auto* mesh = p_drawable.mesh;

    if (mesh == nullptr)
    {
        LogRecordedDrawPreparationState(
            m_driver,
            "overrides",
            "mesh_null",
            p_drawable);
        return false;
    }

    const auto gpuInstances = effectiveMaterial.GetGPUInstances();
    if (!(mesh && gpuInstances > 0))
    {
        LogRecordedDrawPreparationState(m_driver, "overrides", "gpu_instances_zero", p_drawable);
        return false;
    }

    auto commandBuffer = GetActiveExplicitCommandBuffer();
    auto effectivePipelineState = CreatePipelineState();
    effectivePipelineState.depthFunc = depthCompareOverride;
    if (!ApplyAttachmentPipelineStateOverrides(m_frameDescriptor, pipelineOverrides))
    {
        LogRecordedDrawPreparationState(m_driver, "overrides", "attachment_sample_count_mismatch", p_drawable);
        return false;
    }

    if (!pipelineOverrides.hasDepthAttachment.has_value())
    {
        pipelineOverrides.hasDepthAttachment =
            NLS::Render::FrameGraph::ResolveExternalSceneOutputFramebuffer(m_frameDescriptor) != nullptr ||
            NLS::Render::FrameGraph::FrameTargetsSwapchain(m_frameDescriptor);
    }
    const auto* staticBase = ResolvePreparedRecordedDrawStaticBase(
        "overrides",
        p_drawable,
        effectiveMaterial,
        pipelineOverrides,
        depthCompareOverride,
        effectivePipelineState,
        lightMode);
    if (staticBase == nullptr)
        return false;

    PopulatePreparedRecordedDrawFromStaticBase(outDraw, std::move(commandBuffer), p_drawable, *staticBase);
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
        if (preparedDraw.passBindingSet != nullptr)
        {
            preparedDraw.commandBuffer->BindBindingSet(
                NLS::Render::RHI::BindingPointMap::kPassDescriptorSet,
                preparedDraw.passBindingSet);
        }
        preparedDraw.commandBuffer->BindBindingSet(
            NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet,
            preparedDraw.materialBindingSet);
    }
}

void ABaseRenderer::SubmitPreparedDraw(const PreparedRecordedDraw& preparedDraw) const
{
    if (preparedDraw.commandBuffer == nullptr || preparedDraw.mesh == nullptr)
        return;

    preparedDraw.commandBuffer->PushConstants(
        Resources::kIndexedObjectDataPushConstantStageMask,
        0u,
        Resources::kIndexedObjectDataPushConstantSize,
        &preparedDraw.objectConstants);

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

    const auto meshVertexCount = preparedDraw.mesh->GetVertexCount();
    const auto vertexStart = std::min(preparedDraw.vertexStart, meshVertexCount);
    const auto vertexCount = preparedDraw.vertexCount != 0u
        ? std::min(preparedDraw.vertexCount, meshVertexCount - vertexStart)
        : meshVertexCount - vertexStart;
    if (vertexCount > 0u)
        preparedDraw.commandBuffer->Draw(vertexCount, preparedDraw.instanceCount, vertexStart, 0);
}

bool ABaseRenderer::QueueThreadedRecordedDraw(PreparedRecordedDraw preparedDraw)
{
    if (preparedDraw.pipeline == nullptr ||
        preparedDraw.materialBindingSet == nullptr ||
        preparedDraw.mesh == nullptr ||
        preparedDraw.instanceCount == 0u)
    {
        return false;
    }

    Context::RecordedDrawCommandInput drawCommand;
    drawCommand.pipeline = std::move(preparedDraw.pipeline);
    drawCommand.frameBindingSet = std::move(preparedDraw.frameBindingSet);
    drawCommand.objectBindingSet = std::move(preparedDraw.objectBindingSet);
    drawCommand.passBindingSet = std::move(preparedDraw.passBindingSet);
    drawCommand.materialBindingSet = std::move(preparedDraw.materialBindingSet);
    drawCommand.mesh = std::move(preparedDraw.mesh);
    drawCommand.instanceCount = preparedDraw.instanceCount;
    drawCommand.vertexStart = preparedDraw.vertexStart;
    drawCommand.vertexCount = preparedDraw.vertexCount;
    drawCommand.objectConstants = preparedDraw.objectConstants;
    drawCommand.usesObjectIndex = preparedDraw.usesObjectIndex;
    m_threadedRecordedDrawCommands.push_back(std::move(drawCommand));
    return true;
}

void ABaseRenderer::ClearPreparedRecordedDrawStaticBaseCache() const
{
    m_preparedRecordedDrawStaticBaseCache.clear();
    m_preparedRecordedDrawStaticBaseStableIndex.clear();
    m_preparedRecordedDrawStaticBaseLruKeys.clear();
}

#if defined(NLS_ENABLE_TEST_HOOKS)
size_t ABaseRenderer::GetPreparedRecordedDrawStaticBaseCacheSizeForTesting() const
{
    return m_preparedRecordedDrawStaticBaseCache.size();
}

size_t ABaseRenderer::GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting() const
{
    return m_preparedRecordedDrawStaticBaseStableIndex.size();
}

size_t ABaseRenderer::GetPreparedRecordedDrawStaticBaseCacheMaxEntriesForTesting()
{
    return kMaxPreparedRecordedDrawStaticBaseCacheEntries;
}

uint64_t ABaseRenderer::GetPreparedRecordedDrawStaticBaseCacheMaxFrameAgeForTesting()
{
    return kMaxPreparedRecordedDrawStaticBaseCacheFrameAge;
}

size_t ABaseRenderer::GetPreparedRecordedDrawStaticBaseCacheAgeSweepBudgetForTesting()
{
    return kPreparedRecordedDrawStaticBaseCacheAgeSweepBudget;
}

size_t ABaseRenderer::AdvancePreparedRecordedDrawStaticBaseCacheForTesting(const uint64_t frameCount) const
{
    const auto previousSize = m_preparedRecordedDrawStaticBaseCache.size();
    m_preparedRecordedDrawStaticBaseCacheFrame += frameCount;
    TrimPreparedRecordedDrawStaticBaseCache(true);
    return previousSize - m_preparedRecordedDrawStaticBaseCache.size();
}
#endif

RendererStats* ABaseRenderer::GetMutableRendererStats() const
{
    return nullptr;
}

void ABaseRenderer::DrawEntity(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable,
    std::string_view lightMode
)
{
    PreparedRecordedDraw preparedDraw;
    if (!PrepareRecordedDraw(p_pso, p_drawable, lightMode, preparedDraw))
        return;

    BindPreparedGraphicsPipeline(preparedDraw);
    BindPreparedMaterialBindingSet(preparedDraw);
    SubmitPreparedDraw(preparedDraw);
}

void ABaseRenderer::DrawEntity(
    const Entities::Drawable& p_drawable,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride,
    std::string_view lightMode)
{
    auto material = p_drawable.material;
    auto mesh = p_drawable.mesh;

    if (material == nullptr || mesh == nullptr)
    {
        NLS_LOG_ERROR("[ABaseRenderer] DrawEntity: material or mesh is null!");
        return;
    }

    PreparedRecordedDraw preparedDraw;
    if (!PrepareRecordedDraw(p_drawable, pipelineOverrides, depthCompareOverride, lightMode, preparedDraw))
        return;

    BindPreparedGraphicsPipeline(preparedDraw);
    BindPreparedMaterialBindingSet(preparedDraw);
    SubmitPreparedDraw(preparedDraw);
}
}
