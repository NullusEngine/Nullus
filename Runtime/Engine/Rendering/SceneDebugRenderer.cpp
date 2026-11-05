#include "Rendering/DeferredSceneRenderer.h"

#include <algorithm>
#include <any>
#include <filesystem>
#include <stdexcept>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/RHI/Core/RHIPipeline.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Shader.h>
#include <Rendering/Resources/Texture2D.h>
#include <ResourceManagement/ShaderManager.h>
#include <Profiling/Profiler.h>

namespace NLS::Engine::Rendering
{
namespace
{
std::string DebugShaderPath(const char* name)
{
    auto path = Render::Resources::Loaders::ShaderLoader::ResolveTrustedBuiltInEngineShaderPath(name);
    return path.empty() ? (std::filesystem::path("App/Assets/Engine/Shaders") / name).string() : path;
}

bool HasTexture(const Render::Resources::Material& material)
{
    for (const auto& [name, value] : material.GetParameterBlock().Data())
        if (value.type() == typeid(Render::Resources::Texture2D*) &&
            std::any_cast<Render::Resources::Texture2D*>(value) != nullptr) return true;
    return false;
}

Render::Context::TextureResourceAccess TextureAccess(
    const std::shared_ptr<Render::RHI::RHITextureView>& view, bool depth, bool write)
{
    using namespace Render;
    Context::TextureResourceAccess access;
    access.texture = view->GetTexture(); access.subresourceRange = view->GetDesc().subresourceRange;
    access.mode = write ? Context::ResourceAccessMode::Write : Context::ResourceAccessMode::Read;
    access.state = depth ? (write ? RHI::ResourceState::DepthWrite : RHI::ResourceState::DepthRead) :
        (write ? RHI::ResourceState::RenderTarget : RHI::ResourceState::ShaderRead);
    access.stages = depth ? RHI::PipelineStageMask::DepthStencil :
        (write ? RHI::PipelineStageMask::RenderTarget : RHI::PipelineStageMask::FragmentShader);
    access.access = depth ? (write ? RHI::AccessMask::DepthStencilWrite | RHI::AccessMask::DepthStencilRead : RHI::AccessMask::DepthStencilRead) :
        (write ? RHI::AccessMask::ColorAttachmentWrite | RHI::AccessMask::ColorAttachmentRead : RHI::AccessMask::ShaderRead);
    return access;
}

// Diagnostic passes change the output ordering. Rebuild indices and resource
// visibility edges together; retaining the old work-unit indices is unsafe.
void RebuildDebugWorkUnits(Render::Context::RenderScenePackage& package)
{
    namespace C = Render::Context;
    package.parallelCommandWorkUnits.clear(); package.workUnitDependencyEdges.clear();
    for (size_t index = 0; index < package.passCommandInputs.size(); ++index)
    {
        auto& pass = package.passCommandInputs[index];
        // Diagnostics insert new attachment dependencies. Keep all producers on
        // graphics so the reordered package cannot retain stale async-queue waits.
        pass.queueType = Render::RHI::QueueType::Graphics;
        pass.dependencySourceWorkUnitIndex.reset();
        auto units = C::BuildRecordedDrawCommandWorkUnitsForPass(pass, index, package.parallelCommandWorkUnits.size());
        if (units.empty()) continue;
        auto add = [&](C::WorkUnitDependencyEdge edge)
        {
            edge.targetWorkUnitIndex = units.front().workUnitIndex;
            units.front().incomingDependencyEdges.push_back(edge); package.workUnitDependencyEdges.push_back(std::move(edge));
        };
        if (!package.parallelCommandWorkUnits.empty())
        {
            C::WorkUnitDependencyEdge edge;
            edge.sourceWorkUnitIndex = package.parallelCommandWorkUnits.back().workUnitIndex;
            add(edge);
        }
        for (const auto& target : pass.textureResourceAccesses)
            for (auto previous = package.parallelCommandWorkUnits.rbegin(); previous != package.parallelCommandWorkUnits.rend(); ++previous)
            {
                const auto& accesses = previous->commandInput.textureResourceAccesses;
                auto source = std::find_if(accesses.begin(), accesses.end(), [&](const auto& access)
                    { return access.texture == target.texture && access.mode == C::ResourceAccessMode::Write; });
                if (source == accesses.end()) continue;
                C::WorkUnitDependencyEdge edge;
                edge.sourceWorkUnitIndex = previous->workUnitIndex; edge.resourceKind = C::ThreadedDependencyResourceKind::Texture;
                edge.sourceTextureAccess = *source; edge.targetTextureAccess = target; add(edge); break;
            }
        for (const auto& target : pass.bufferResourceAccesses)
            for (auto previous = package.parallelCommandWorkUnits.rbegin(); previous != package.parallelCommandWorkUnits.rend(); ++previous)
            {
                const auto& accesses = previous->commandInput.bufferResourceAccesses;
                auto source = std::find_if(accesses.begin(), accesses.end(), [&](const auto& access)
                    { return access.buffer == target.buffer && access.mode == C::ResourceAccessMode::Write; });
                if (source == accesses.end()) continue;
                C::WorkUnitDependencyEdge edge;
                edge.sourceWorkUnitIndex = previous->workUnitIndex; edge.resourceKind = C::ThreadedDependencyResourceKind::Buffer;
                edge.sourceBufferAccess = *source; edge.targetBufferAccess = target; add(edge); break;
            }
        for (size_t slice = 1; slice < units.size(); ++slice)
        {
            C::WorkUnitDependencyEdge edge;
            edge.sourceWorkUnitIndex = units[slice - 1].workUnitIndex; edge.targetWorkUnitIndex = units[slice].workUnitIndex;
            units[slice].incomingDependencyEdges.push_back(edge); package.workUnitDependencyEdges.push_back(edge);
        }
        package.parallelCommandWorkUnits.insert(package.parallelCommandWorkUnits.end(),
            std::make_move_iterator(units.begin()), std::make_move_iterator(units.end()));
    }
    package.hasAsyncComputeWorkload = false; package.asyncComputeWorkloadCount = 0;
    package.containsParallelCommandWorkUnits = !package.parallelCommandWorkUnits.empty();
    package.parallelCommandWorkUnitCount = package.parallelCommandWorkUnits.size();
    package.parallelDrawCommandBatches = C::BuildParallelDrawCommandBatchMetadata(package.parallelCommandWorkUnits);
    package.passPlanCount = package.passCommandInputs.size();
}
}

Render::Resources::Material& DeferredSceneRenderer::ResolveSceneDebugMaterial(
    Render::Resources::Material& source, const SceneRenderOptions& options,
    const bool forward, const bool transparent, const bool wire, const Maths::Vector4 wireColor)
{
    using namespace Render::Resources;
    auto* pass = source.ResolveShaderForLightMode("SceneDebug");
    const auto* sourceShader = source.GetShader();
    const std::array<uint64_t, 7u> sourceStamp {
        source.GetParameterRevision(), source.GetRenderStateRevision(), source.GetBindingRevision(),
        sourceShader ? sourceShader->GetInstanceId() : 0u, sourceShader ? sourceShader->GetGeneration() : 0u,
        pass ? pass->GetInstanceId() : 0u, pass ? pass->GetGeneration() : 0u
    };
    const auto sourceId = source.GetInstanceId();
    if (!m_sceneDebugMaterialCache.contains(sourceId) && m_sceneDebugMaterialCache.size() >= 1024u)
        m_sceneDebugMaterialCache.clear();
    const size_t variant = (forward ? 1u : 0u) | (transparent ? 2u : 0u) | (wire ? 4u : 0u);
    auto& entry = m_sceneDebugMaterialCache[sourceId][variant];
    const bool rebuild = !entry.material || entry.sourceStamp != sourceStamp;
    if (rebuild)
    {
        std::unique_ptr<Material> material;
        bool supported = pass != nullptr;
        if (pass)
            material = source.CreateDynamicCopy();
        else
        {
            if (!m_sceneDebugShader)
                m_sceneDebugShader = Loaders::ShaderLoader::CreateBuiltInHlsl(DebugShaderPath("SceneDebugFallback.hlsl"),
                    Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());
            if (!m_sceneDebugShader) throw std::runtime_error("Scene diagnostic shader is unavailable");
            material = std::make_unique<Material>(m_sceneDebugShader);
            SyncGBufferMaterial(*material, source);
            // Preserve authored zero values after the legacy fallback defaults.
            for (const auto& [name, value] : source.GetParameterBlock().Data())
                if (material->GetParameterBlock().Contains(name)) material->SetRawParameter(name, value);
            supported = source.GetParameterBlock().Contains("u_Albedo") || source.GetParameterBlock().Contains("u_Diffuse");
        }
        const bool pbr = pass ? !pass->GetMaterialInterface()->unlit :
            supported && source.GetParameterBlock().Contains("u_Metallic");
        entry.material = std::move(material);
        entry.sourceStamp = sourceStamp;
        entry.displayOptions = Maths::Vector4(0, 0, pbr ? 1.0f : (supported ? 0.0f : -1.0f), HasTexture(source) ? 1.0f : 0.0f);
    }
    const Maths::Vector4 displayOptions(wire ? 11.0f : float(options.debugMode),
        (forward ? 1.0f : 0.0f) + (transparent ? 2.0f : 0.0f), entry.displayOptions.z, entry.displayOptions.w);
    // SetRawParameter invalidates explicit bindings even for equal values.
    // Leave stable diagnostic draws untouched so their layouts, PSOs and
    // prepared draw entries can survive across frames.
    if (rebuild || entry.displayOptions != displayOptions)
    {
        entry.material->SetRawParameter("u_SceneDebugOptions", displayOptions);
        entry.displayOptions = displayOptions;
    }
    if (rebuild || entry.albedoRange != options.debugParameters.albedoRange)
    {
        entry.material->SetRawParameter("u_SceneAlbedoRange", options.debugParameters.albedoRange);
        entry.albedoRange = options.debugParameters.albedoRange;
    }
    if (rebuild || entry.specularRange != options.debugParameters.specularRange)
    {
        entry.material->SetRawParameter("u_SceneSpecularRange", options.debugParameters.specularRange);
        entry.specularRange = options.debugParameters.specularRange;
    }
    if (rebuild || entry.wireColor != wireColor)
    {
        entry.material->SetRawParameter("u_SceneWireColor", wireColor);
        entry.wireColor = wireColor;
    }
    return *entry.material;
}

Render::Resources::Material& DeferredSceneRendererTestAccess::ResolveSceneDebugMaterialForTesting(
    DeferredSceneRenderer& renderer, Render::Resources::Material& source, const SceneRenderOptions& options,
    const bool forward, const bool transparent, const bool wire, const Maths::Vector4 wireColor)
{
    return renderer.ResolveSceneDebugMaterial(source, options, forward, transparent, wire, wireColor);
}

void DeferredSceneRenderer::AppendSceneDebugPasses(Render::Context::RenderScenePackage& package,
    const Render::FrameGraph::FrozenFrameDescriptor& frame, const AllDrawables& drawables)
{
    NLS_PROFILE_SCOPE();
    namespace R = Render::RHI;
    namespace C = Render::Context;
    const auto& options = drawables.viewOptions;
    const bool wire = options.geometryMode == SceneGeometryDrawMode::Wireframe;
    if (!wire && (options.debugMode == SceneDisplayDebugMode::None || Render::Debug::IsGBufferDebugMode(options.debugMode))) return;
    auto device = C::DriverRendererAccess::GetExplicitDevice(m_driver);
    auto output = package.externalSceneOutputColorView;
    if (!output)
        output = Render::FrameGraph::ResolveExternalSceneOutputAttachments(frame.descriptor, "SceneDebugOutput", "SceneDebugOutputDepth").colorView;
    if (!device || !output) throw std::runtime_error("Scene diagnostics require an offscreen view output");
    R::RHITextureDesc desc;
    desc.extent = {package.renderWidth, package.renderHeight, 1}; desc.format = R::TextureFormat::RGBA16F;
    desc.usage = R::TextureUsageFlags::ColorAttachment | R::TextureUsageFlags::Sampled;
    desc.debugName = "SceneDebugAccumulator";
    auto texture = device->CreateTexture(desc);
    R::RHITextureViewDesc viewDesc; viewDesc.format = desc.format; viewDesc.debugName = "SceneDebugAccumulatorView";
    auto color = texture ? device->CreateTextureView(texture, viewDesc) : nullptr;
    if (!color) throw std::runtime_error("Cannot create the scene diagnostic accumulator");
    const bool overdraw = options.debugMode == SceneDisplayDebugMode::Overdraw;
    std::shared_ptr<R::RHITextureView> depth;
    if (!overdraw)
    {
        if (wire) depth = LoadPublishedGBufferDepthView();
        if (!depth)
        {
            auto depthDesc = desc; depthDesc.format = Render::FrameGraph::kDeferredGBufferDepthFormat;
            depthDesc.usage = R::TextureUsageFlags::DepthStencilAttachment | R::TextureUsageFlags::Sampled;
            depthDesc.debugName = "SceneDebugDepth";
            auto depthTexture = device->CreateTexture(depthDesc);
            auto depthViewDesc = viewDesc; depthViewDesc.format = depthDesc.format;
            depth = depthTexture ? device->CreateTextureView(depthTexture, depthViewDesc) : nullptr;
        }
        if (!depth) throw std::runtime_error("Cannot create scene diagnostic depth");
    }
    C::RenderPassCommandInput geometry;
    geometry.kind = C::RenderPassCommandKind::Opaque; geometry.debugName = "SceneDebugGeometry";
    geometry.renderWidth = package.renderWidth; geometry.renderHeight = package.renderHeight;
    geometry.usesColorAttachment = true; geometry.colorAttachmentViews = {color}; geometry.clearColor = true; geometry.clearColorValue = {0,0,0,0};
    geometry.usesDepthStencilAttachment = depth != nullptr && !overdraw;
    geometry.writesDepthStencilAttachment = geometry.usesDepthStencilAttachment;
    geometry.clearDepth = geometry.usesDepthStencilAttachment; geometry.depthStencilAttachmentView = geometry.usesDepthStencilAttachment ? depth : nullptr;
    geometry.textureResourceAccesses.push_back(TextureAccess(color, false, true));
    if (geometry.usesDepthStencilAttachment) geometry.textureResourceAccesses.push_back(TextureAccess(depth, true, true));
    auto capture = [&](const auto& entries, bool transparent)
    {
        for (const auto& entry : entries)
        {
            const auto& drawable = entry.second;
            if (!drawable.material || !drawable.mesh) continue;
            const auto* shader = drawable.material->GetShader();
            const bool forward = transparent || (shader && shader->GetMaterialInterface()->forwardOpaque);
            auto* material = &ResolveSceneDebugMaterial(*drawable.material, options, forward, transparent, wire);
            Render::Resources::MaterialPipelineStateOverrides overrides;
            const R::TextureFormat format[]{desc.format}; overrides.SetColorFormats(format);
            const R::TextureColorSpace spaces[]{R::TextureColorSpace::Linear}; overrides.SetColorSpaces(spaces);
            overrides.hasDepthAttachment = geometry.usesDepthStencilAttachment;
            overrides.depthFormat = Render::FrameGraph::kDeferredGBufferDepthFormat;
            overrides.depthTest = geometry.usesDepthStencilAttachment; overrides.depthWrite = geometry.usesDepthStencilAttachment && !transparent;
            overrides.stencilTest = false; overrides.wireframe = wire;
            R::RHIRenderTargetBlendStateDesc blend;
            blend.blendEnable = overdraw || (transparent && options.debugMode == SceneDisplayDebugMode::AlphaChannel);
            if (overdraw) blend.srcColor = blend.dstColor = blend.srcAlpha = blend.dstAlpha = R::RHIBlendFactor::One;
            else if (blend.blendEnable)
            {
                const auto state = shader ? shader->GetShaderLabPassState() : std::nullopt;
                if (state && !state->blend.renderTargets.empty()) blend = state->blend.renderTargets.front();
                blend.blendEnable = true; blend.srcAlpha = R::RHIBlendFactor::One;
            }
            overrides.blending = blend.blendEnable; overrides.SetRenderTargetBlendStates(std::span(&blend,1));
            PreparedRecordedDraw prepared;
            if (!CaptureThreadedPreparedDraw(drawable, *material, overrides, Render::Settings::EComparaisonAlgorithm::LESS_EQUAL, GetSceneDebugLightMode(*material), prepared) ||
                !QueueThreadedRecordedDraw(std::move(prepared), geometry.recordedDrawCommands))
                throw std::runtime_error("Cannot capture a scene diagnostic draw");
        }
    };
    capture(drawables.opaques, false);
    geometry.drawCount = geometry.recordedDrawCommands.size();
    std::vector<C::RenderPassCommandInput> diagnostics;
    auto opaque = geometry;
    geometry.recordedDrawCommands.clear();
    capture(drawables.transparents, true);
    geometry.kind = C::RenderPassCommandKind::Transparent; geometry.debugName = "SceneDebugTransparents";
    geometry.clearColor = false; geometry.clearDepth = false; geometry.writesDepthStencilAttachment = false;
    if (geometry.usesDepthStencilAttachment) geometry.textureResourceAccesses[1] = TextureAccess(depth, true, false);
    geometry.drawCount = geometry.recordedDrawCommands.size();
    diagnostics.push_back(std::move(opaque)); diagnostics.push_back(std::move(geometry));
    if (options.includeParticles && !drawables.particles.empty())
    {
        C::RenderScenePackage particles;
        particles.renderWidth = package.renderWidth; particles.renderHeight = package.renderHeight; particles.targetsSwapchain = false;
        particles.externalSceneOutputColorView = color; particles.externalSceneOutputDepthView = overdraw ? nullptr : depth;
        particles.clearColorValue = {0,0,0,0}; particles.passCommandInputs = diagnostics;
        AppendParticleScenePass(particles, frame, drawables.particles, options);
        diagnostics = std::move(particles.passCommandInputs);
    }
    // FinalizePreparedDeferredScenePackage retargets ordinary transparent passes
    // to the view output. These already own the floating-point attachment.
    for (auto& pass : diagnostics) pass.kind = C::RenderPassCommandKind::Opaque;
    if (!m_sceneDebugResolveShader)
        m_sceneDebugResolveShader = Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(DebugShaderPath("SceneDebugResolve.hlsl"),
            Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());
    if (!m_sceneDebugResolveShader) throw std::runtime_error("Scene diagnostic resolve shader is unavailable");
    auto wrapped = Render::Resources::Texture2D::WrapExternal(texture, package.renderWidth, package.renderHeight);
    Render::Resources::Material resolve(m_sceneDebugResolveShader);
    resolve.SetRawParameter("u_SceneDebugTexture", wrapped.get());
    resolve.SetRawParameter("u_ResolveMode", wire ? 11 : int(options.debugMode));
    resolve.SetRawParameter("u_OverdrawMax", options.debugParameters.overdrawMax);
    Drawable quad; quad.mesh = m_fullscreenQuad.get(); quad.material = &resolve;
    Render::Resources::MaterialPipelineStateOverrides overrides;
    const R::TextureFormat formats[]{output->GetDesc().format}; overrides.SetColorFormats(formats);
    const R::TextureColorSpace spaces[]{output->GetDesc().colorSpace}; overrides.SetColorSpaces(spaces);
    overrides.depthTest = false; overrides.depthWrite = false; overrides.hasDepthAttachment = false;
    overrides.culling = false; overrides.wireframe = false; overrides.blending = false;
    C::RenderPassCommandInput resolved; resolved.kind = C::RenderPassCommandKind::Helper; resolved.debugName = "SceneDebugResolve";
    resolved.renderWidth = package.renderWidth; resolved.renderHeight = package.renderHeight;
    resolved.usesColorAttachment = true; resolved.colorAttachmentViews = {output};
    resolved.textureResourceAccesses = {TextureAccess(color, false, false), TextureAccess(output, false, true)};
    PreparedRecordedDraw prepared;
    if (!CaptureThreadedPreparedDraw(quad, resolve, overrides, Render::Settings::EComparaisonAlgorithm::ALWAYS, {}, prepared) ||
        !QueueThreadedRecordedDraw(std::move(prepared), resolved.recordedDrawCommands)) throw std::runtime_error("Cannot capture scene diagnostic resolve");
    resolved.drawCount = 1; diagnostics.push_back(std::move(resolved));
    auto helpers = std::find_if(package.passCommandInputs.begin(), package.passCommandInputs.end(), [](const auto& pass)
        { return pass.kind == C::RenderPassCommandKind::Helper; });
    package.passCommandInputs.insert(helpers, std::make_move_iterator(diagnostics.begin()), std::make_move_iterator(diagnostics.end()));
    for (auto& pass : package.passCommandInputs)
        if (pass.kind == C::RenderPassCommandKind::Helper && pass.colorAttachmentViews == std::vector{output}) pass.clearColor = false;
    package.hasVisibleDraws = true; package.containsCommandInputs = true;
    package.drawCommandCount = 0;
    for (const auto& pass : package.passCommandInputs) package.drawCommandCount += pass.drawCount;
    RebuildDebugWorkUnits(package);
}
}
