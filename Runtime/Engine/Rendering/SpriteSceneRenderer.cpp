#include "Rendering/SpriteSceneRenderer.h"
#include "Components/SpriteRendererComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include "Assets/RuntimeAssetDatabase.h"
#include "Assets/SpriteProjectSettings.h"
#include <Core/ResourceManagement/MaterialManager.h>
#include <Core/ResourceManagement/TextureManager.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ResourceManagement/ResourceLifetimeRegistry.h>
#include <Core/ServiceLocator.h>
#include <Assets/SpriteGeometry.h>
#include <Rendering/Resources/Shader.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Texture2D.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Settings/EGraphicsBackend.h>
#include <Profiling/Profiler.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace NLS::Engine::Rendering
{
namespace rhi = Render::RHI;
namespace context = Render::Context;
namespace
{
struct SharedLeaseKey
{
    const void* registry = nullptr;
    const void* manager = nullptr;
    const void* resource = nullptr;
    uint64_t resourceId = 0;
    std::string path;
    bool operator==(const SharedLeaseKey&) const = default;
};
struct SharedLeaseKeyHash
{
    size_t operator()(const SharedLeaseKey& key) const
    {
        auto hash = std::hash<const void*>{}(key.registry);
        hash ^= std::hash<const void*>{}(key.manager) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        hash ^= std::hash<const void*>{}(key.resource) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        hash ^= std::hash<uint64_t>{}(key.resourceId) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        return hash ^ (std::hash<std::string>{}(key.path) + 0x9e3779b9u + (hash << 6) + (hash >> 2));
    }
};
std::atomic<uint64_t> nextSharedLeaseId {1};

template<class Resource, class Manager, class Acquire>
std::shared_ptr<Core::ResourceManagement::ResourceHandle<Resource>> CaptureSharedResourceOwner(
    Core::ResourceManagement::ResourceLifetimeRegistry& registry, Manager& manager, Resource& resource,
    const std::string& path, Acquire acquire)
{
    using Handle = Core::ResourceManagement::ResourceHandle<Resource>;
    static std::mutex mutex;
    static std::unordered_map<SharedLeaseKey, std::weak_ptr<Handle>, SharedLeaseKeyHash> cache;
    static uint64_t accesses = 0;
    std::lock_guard lock(mutex);
    // The cache never owns a lease. Scene and in-flight frame snapshots retain
    // it together; the last snapshot releases it after GPU retirement.
    if ((++accesses & 1023u) == 0)
        std::erase_if(cache, [](const auto& entry) { return entry.second.expired(); });
    auto& weak = cache[{&registry, &manager, &resource, uint64_t(resource.GetInstanceID()), path}];
    if (auto existing = weak.lock(); existing && registry.IsGenerationCurrent(existing->Id()))
        return existing;
    auto handle = std::make_shared<Handle>(acquire("SpriteSnapshot:" + std::to_string(nextSharedLeaseId.fetch_add(1))));
    if (!handle->IsValid()) return {};
    weak = handle;
    return handle;
}

Render::Resources::Material* ResolveDefaultSpriteMaterial(std::string& resourcePath, std::string& error)
{
    constexpr auto sourcePath = "Assets/Engine/Shaders/ShaderLab/SpriteUnlit.shader";
    const Core::Assets::AssetId shaderId(Guid::NewDeterministic(std::string("nullus-engine-asset:") + sourcePath));
    const auto* entry = NLS_SERVICE(Assets::RuntimeAssetDatabase).Resolve(shaderId, "shader:SpriteUnlit");
    if (!entry || entry->artifactType != Core::Assets::ArtifactType::Shader)
    { error = "The built-in SpriteUnlit shader artifact is missing from the runtime manifest"; return nullptr; }
    auto* shader = NLS_SERVICE(Core::ResourceManagement::ShaderManager).GetResource(entry->artifactPath);
    if (!shader) { error = "The built-in SpriteUnlit shader artifact could not be loaded"; return nullptr; }
    // Like the default 3D material, this is a generated material over a compiled
    // ShaderLab pass. Runtime loading never reads an authoring .mat or .shader.
    resourcePath = ":Generated/SpriteUnlit/" + entry->contentHash.substr(entry->contentHash.find(':') + 1);
    auto& manager = NLS_SERVICE(Core::ResourceManagement::MaterialManager);
    if (auto* existing = manager.GetResource(resourcePath, false)) return existing;
    auto material = std::make_unique<Render::Resources::Material>();
    material->path = resourcePath;
    material->SetShaderLabSourcePath(shader->GetImportedArtifactSourcePath());
    material->RegisterShaderLabPassShader(shader);
    material->SetRawParameter("_BaseColor", Maths::Vector4(1,1,1,1));
    material->SetSurfaceMode(Render::Resources::MaterialSurfaceMode::Transparent);
    material->SetBlendable(true);
    material->SetDepthTest(true);
    material->SetDepthWriting(false);
    material->SetBackfaceCulling(false);
    material->SetFrontfaceCulling(false);
    return manager.RegisterResource(resourcePath, material.release());
}

std::shared_ptr<const SpriteShaderSnapshot> CaptureProgram(Render::Resources::Material& material, std::string& error)
{
    auto* shader = material.GetShader();
    if (!shader) { error = "Sprite material has no shader"; return {}; }
    const auto keywords = material.GetShaderLabKeywords().Hash();
    const auto backend = Render::Context::TryGetLocatedActiveGraphicsBackend();
    const auto target = backend && (*backend == Render::Settings::EGraphicsBackend::VULKAN || *backend == Render::Settings::EGraphicsBackend::METAL)
        ? Render::ShaderCompiler::ShaderTargetPlatform::SPIRV : Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    const auto identity = std::to_string(material.GetInstanceId()) + ":" + std::to_string(material.GetParameterRevision()) + ":" +
        std::to_string(shader->GetInstanceId()) + ":" + std::to_string(shader->GetGeneration()) + ":" +
        std::to_string(keywords) + ":" + std::to_string(static_cast<int>(target));
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<const SpriteShaderSnapshot>> cache;
    static std::atomic<uint64_t> nextIdentity {1};
    std::lock_guard lock(mutex);
    std::erase_if(cache, [](const auto& item) { return item.second.expired(); });
    if (auto found = cache[identity].lock()) return found;
    auto program = std::make_shared<SpriteShaderSnapshot>(); program->identity = nextIdentity.fetch_add(1);
    for (const auto& stage : shader->GetCompiledArtifacts())
    {
        if (stage.keywordHash != keywords || stage.output.bytecode.empty() ||
            (stage.targetPlatform != Render::ShaderCompiler::ShaderTargetPlatform::DXIL && stage.targetPlatform != Render::ShaderCompiler::ShaderTargetPlatform::SPIRV) ||
            (stage.entryPoint != "SpriteVS" && stage.entryPoint != "SpritePS")) continue;
        rhi::RHIShaderModuleDesc module;
        module.entryPoint = stage.entryPoint; module.bytecode = stage.output.bytecode;
        module.stage = stage.stage == Render::ShaderCompiler::ShaderStage::Vertex ? rhi::ShaderStage::Vertex : rhi::ShaderStage::Fragment;
        module.targetBackend = stage.targetPlatform == Render::ShaderCompiler::ShaderTargetPlatform::DXIL
            ? rhi::NativeBackendType::DX12 : rhi::NativeBackendType::Vulkan;
        program->shaders.push_back(module);
        if (module.targetBackend == rhi::NativeBackendType::Vulkan)
        { module.targetBackend = rhi::NativeBackendType::Metal; program->shaders.push_back(std::move(module)); }
    }
    if (program->shaders.empty()) { error = "Material is missing SpriteVS/SpritePS; select a Sprite material"; return {}; }
    const auto reflection = shader->HasVariantReflections() ? shader->GetReflectionSnapshot(target, keywords) : shader->GetReflectionSnapshot();
    if (!reflection) { error = "Sprite material reflection is missing"; return {}; }
    for (const auto& buffer : reflection->constantBuffers)
        if (buffer.bindingSpace == rhi::BindingPointMap::kMaterialBindingSpace)
        {
            SpriteShaderSnapshot::Constant constant; constant.binding = buffer.bindingIndex;
            if (!Render::Resources::CaptureMaterialConstantBuffer(buffer, material.GetParameterBlock(), constant.bytes, error)) return {};
            program->constants.push_back(std::move(constant));
            program->materialLayout.entries.push_back({buffer.name, rhi::BindingType::UniformBuffer,
                rhi::BindingPointMap::kMaterialDescriptorSet, buffer.bindingIndex, 1, rhi::ShaderStageMask::AllGraphics,
                rhi::BindingPointMap::kMaterialBindingSpace, 0});
        }
    cache[identity] = program;
    return program;
}

using PackedSprite = std::array<Maths::Vector4, 6>;
PackedSprite Pack(const SpriteRenderProxy& source,const std::array<float,4>& rect,const std::array<float,4>& uv)
{
    const float sx = source.flipX ? -1.f : 1.f, sy = source.flipY ? -1.f : 1.f;
    PackedSprite result;
    result[0] = source.world * Maths::Vector4(rect[0]*sx, rect[1]*sy, 0, 1);
    result[1] = source.world * Maths::Vector4(rect[0]*sx, rect[3]*sy, 0, 1);
    result[2] = source.world * Maths::Vector4(rect[2]*sx, rect[3]*sy, 0, 1);
    result[3] = source.world * Maths::Vector4(rect[2]*sx, rect[1]*sy, 0, 1);
    result[4] = {uv[0],uv[1],uv[2],uv[3]}; result[5] = source.color;
    return result;
}
bool Visible(const PackedSprite& sprite, const Maths::Matrix4& vp)
{
    std::array<bool, 6> outside {true,true,true,true,true,true};
    for (size_t index = 0; index < 4; ++index)
    {
        const auto clip = vp * sprite[index];
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z) || !std::isfinite(clip.w)) return false;
        outside[0] &= clip.x < -clip.w; outside[1] &= clip.x > clip.w;
        outside[2] &= clip.y < -clip.w; outside[3] &= clip.y > clip.w;
        outside[4] &= clip.z < -clip.w; outside[5] &= clip.z > clip.w;
    }
    return std::none_of(outside.begin(), outside.end(), [](bool value) { return value; });
}

// Preserve every existing work-unit edge while inserting the new graphics pass.
void InsertPass(context::RenderScenePackage& package, context::RenderPassCommandInput pass, size_t passIndex)
{
    if (package.parallelCommandWorkUnits.empty())
        for (size_t index = 0; index < package.passCommandInputs.size(); ++index)
        {
            auto units = context::BuildRecordedDrawCommandWorkUnitsForPass(package.passCommandInputs[index], index, package.parallelCommandWorkUnits.size());
            package.parallelCommandWorkUnits.insert(package.parallelCommandWorkUnits.end(), std::make_move_iterator(units.begin()), std::make_move_iterator(units.end()));
        }
    size_t unitIndex = package.parallelCommandWorkUnits.size();
    for (size_t i = 0; i < package.parallelCommandWorkUnits.size(); ++i)
        if (package.parallelCommandWorkUnits[i].sourcePassIndex >= passIndex &&
            package.parallelCommandWorkUnits[i].sourcePassIndex != context::kInvalidParallelCommandSourcePassIndex) { unitIndex = i; break; }
    auto units = context::BuildRecordedDrawCommandWorkUnitsForPass(pass, passIndex, unitIndex);
    if (units.empty()) return;
    const auto remap = [&](uint64_t& index) { if (index >= unitIndex) index += units.size(); };
    for (auto& previous : package.parallelCommandWorkUnits)
    {
        remap(previous.workUnitIndex); remap(previous.submissionOrder);
        if (previous.sourcePassIndex >= passIndex && previous.sourcePassIndex != context::kInvalidParallelCommandSourcePassIndex) ++previous.sourcePassIndex;
        if (previous.sourcePassIndex > passIndex && previous.sourcePassIndex != context::kInvalidParallelCommandSourcePassIndex)
        {
            auto& successor = previous.commandInput;
            if (pass.clearColor && successor.colorAttachmentViews == pass.colorAttachmentViews && successor.targetsSwapchain == pass.targetsSwapchain) successor.clearColor = false;
            if (pass.clearDepth && successor.depthStencilAttachmentView == pass.depthStencilAttachmentView && successor.targetsSwapchain == pass.targetsSwapchain) successor.clearDepth = false;
            if (pass.clearStencil && successor.depthStencilAttachmentView == pass.depthStencilAttachmentView && successor.targetsSwapchain == pass.targetsSwapchain) successor.clearStencil = false;
        }
        if (previous.commandInput.dependencySourceWorkUnitIndex) remap(*previous.commandInput.dependencySourceWorkUnitIndex);
        for (auto& edge : previous.incomingDependencyEdges) { remap(edge.sourceWorkUnitIndex); remap(edge.targetWorkUnitIndex); }
    }
    for (auto& previous : package.passCommandInputs)
        if (previous.dependencySourceWorkUnitIndex) remap(*previous.dependencySourceWorkUnitIndex);
    for (auto& edge : package.workUnitDependencyEdges) { remap(edge.sourceWorkUnitIndex); remap(edge.targetWorkUnitIndex); }
    auto connect = [&](uint64_t from, context::ParallelCommandWorkUnit& to)
    {
        context::WorkUnitDependencyEdge edge; edge.sourceWorkUnitIndex = from; edge.targetWorkUnitIndex = to.workUnitIndex;
        edge.resourceKind = context::ThreadedDependencyResourceKind::Texture;
        edge.sourceTextureAccess = pass.textureResourceAccesses.front(); edge.targetTextureAccess = pass.textureResourceAccesses.front();
        to.incomingDependencyEdges.push_back(edge); package.workUnitDependencyEdges.push_back(std::move(edge));
    };
    if (unitIndex > 0) connect(package.parallelCommandWorkUnits[unitIndex-1].workUnitIndex, units.front());
    if (pass.depthStencilAttachmentView && !pass.writesDepthStencilAttachment)
        for (size_t i = unitIndex; i > 0; --i)
        {
            const auto& previous = package.parallelCommandWorkUnits[i-1];
            if (previous.commandInput.depthStencilAttachmentView != pass.depthStencilAttachmentView || !previous.commandInput.writesDepthStencilAttachment) continue;
            context::WorkUnitDependencyEdge edge; edge.sourceWorkUnitIndex = previous.workUnitIndex; edge.targetWorkUnitIndex = units.front().workUnitIndex;
            edge.resourceKind = context::ThreadedDependencyResourceKind::Texture;
            auto source = pass.textureResourceAccesses[1]; source.mode = context::ResourceAccessMode::Write;
            source.state = rhi::ResourceState::DepthWrite; source.access = rhi::AccessMask::DepthStencilWrite;
            edge.sourceTextureAccess = source; edge.targetTextureAccess = pass.textureResourceAccesses[1];
            units.front().incomingDependencyEdges.push_back(edge); package.workUnitDependencyEdges.push_back(std::move(edge)); break;
        }
    if (unitIndex < package.parallelCommandWorkUnits.size()) connect(units.back().workUnitIndex, package.parallelCommandWorkUnits[unitIndex]);
    for (size_t i = passIndex; i < package.passCommandInputs.size(); ++i)
    {
        auto& successor = package.passCommandInputs[i];
        if (pass.clearColor && successor.colorAttachmentViews == pass.colorAttachmentViews && successor.targetsSwapchain == pass.targetsSwapchain) successor.clearColor = false;
        if (pass.clearDepth && successor.depthStencilAttachmentView == pass.depthStencilAttachmentView && successor.targetsSwapchain == pass.targetsSwapchain) successor.clearDepth = false;
        if (pass.clearStencil && successor.depthStencilAttachmentView == pass.depthStencilAttachmentView && successor.targetsSwapchain == pass.targetsSwapchain) successor.clearStencil = false;
    }
    package.parallelCommandWorkUnits.insert(package.parallelCommandWorkUnits.begin()+unitIndex, std::make_move_iterator(units.begin()), std::make_move_iterator(units.end()));
    package.containsParallelCommandWorkUnits = true; package.parallelCommandWorkUnitCount = package.parallelCommandWorkUnits.size();
    package.parallelDrawCommandBatches = context::BuildParallelDrawCommandBatchMetadata(package.parallelCommandWorkUnits);
    package.passCommandInputs.insert(package.passCommandInputs.begin()+passIndex, std::move(pass));
    package.passPlanCount = package.passCommandInputs.size();
}

bool SameOutputView(const std::shared_ptr<rhi::RHITextureView>& a, const std::shared_ptr<rhi::RHITextureView>& b)
{
    if (a == b) return true;
    if (!a || !b || a->GetTexture() != b->GetTexture()) return false;
    const auto& x = a->GetDesc().subresourceRange;
    const auto& y = b->GetDesc().subresourceRange;
    return x.baseMipLevel == y.baseMipLevel && x.mipLevelCount == y.mipLevelCount &&
        x.baseArrayLayer == y.baseArrayLayer && x.arrayLayerCount == y.arrayLayerCount;
}

void EnsureSceneOutputClears(context::RenderScenePackage& package, const Render::FrameGraph::FrozenFrameDescriptor& view)
{
    if (!package.clearColorBuffer && !package.clearDepthBuffer && !package.clearStencilBuffer) return;
    auto color = package.externalSceneOutputColorView;
    auto depth = package.externalSceneOutputDepthView;
    if (!color && !package.targetsSwapchain)
    {
        const auto outputs = Render::FrameGraph::ResolveExternalSceneOutputAttachments(
            view.descriptor, "SpriteSceneClearColor", "SpriteSceneClearDepth");
        color = outputs.colorView; depth = outputs.depthStencilView;
    }
    const bool swapchain = package.targetsSwapchain && !color;
    bool clearColor = package.clearColorBuffer && (color || swapchain);
    bool clearDepth = package.clearDepthBuffer && bool(depth);
    bool clearStencil = package.clearStencilBuffer && depth && depth->GetDesc().format == rhi::TextureFormat::Depth24Stencil8;
    bool sawColor = false, sawDepth = false;
    size_t insertion = package.passCommandInputs.size();
    for (size_t index = 0; index < package.passCommandInputs.size(); ++index)
    {
        const auto& pass = package.passCommandInputs[index];
        if (!sawColor && pass.usesColorAttachment &&
            ((swapchain && pass.targetsSwapchain) || (color && std::any_of(pass.colorAttachmentViews.begin(), pass.colorAttachmentViews.end(),
                [&](const auto& attachment) { return SameOutputView(attachment, color); }))))
        {
            sawColor = true;
            clearColor &= !pass.clearColor;
            if (clearColor) insertion = (std::min)(insertion, index);
        }
        if (!sawDepth && pass.usesDepthStencilAttachment &&
            ((swapchain && pass.targetsSwapchain) || (depth && SameOutputView(pass.depthStencilAttachmentView, depth))))
        {
            sawDepth = true;
            clearDepth &= !pass.clearDepth; clearStencil &= !pass.clearStencil;
            if (clearDepth || clearStencil) insertion = (std::min)(insertion, index);
        }
    }
    if (!clearColor && !clearDepth && !clearStencil) return;
    // Helper-only Deferred views have no opaque pass to initialize their
    // retained framebuffer. Initialize it before grid/gizmo draws, including
    // frames where the last Sprite is culled or removed.
    context::RenderPassCommandInput clear;
    clear.kind = context::RenderPassCommandKind::Opaque; clear.debugName = "SceneOutputClear";
    clear.renderWidth = package.renderWidth; clear.renderHeight = package.renderHeight;
    clear.outputViewport = view.descriptor.outputViewport;
    clear.targetsSwapchain = swapchain; clear.clearColorValue = package.clearColorValue;
    clear.clearColor = clearColor; clear.clearDepth = clearDepth; clear.clearStencil = clearStencil;
    clear.usesColorAttachment = clearColor;
    clear.usesDepthStencilAttachment = clear.writesDepthStencilAttachment = clearDepth || clearStencil;
    if (clearColor)
    {
        if (color) clear.colorAttachmentViews = {color};
        clear.textureResourceAccesses.push_back({color ? color->GetTexture() : nullptr, color ? color->GetDesc().subresourceRange : rhi::RHISubresourceRange{},
            context::ResourceAccessMode::Write, rhi::ResourceState::RenderTarget, rhi::PipelineStageMask::RenderTarget, rhi::AccessMask::ColorAttachmentWrite});
    }
    if (clearDepth || clearStencil)
    {
        clear.depthStencilAttachmentView = depth;
        clear.textureResourceAccesses.push_back({depth ? depth->GetTexture() : nullptr, depth ? depth->GetDesc().subresourceRange : rhi::RHISubresourceRange{},
            context::ResourceAccessMode::Write, rhi::ResourceState::DepthWrite, rhi::PipelineStageMask::DepthStencil, rhi::AccessMask::DepthStencilWrite});
    }
    InsertPass(package, std::move(clear), insertion);
    package.containsCommandInputs = true;
}
}

namespace
{
struct SpriteCaptureResources
{
    Components::SpriteAssetCaptureContext sprites;
    struct Texture
    {
        std::shared_ptr<Core::ResourceManagement::ResourceHandle<Render::Resources::Texture2D>> owner;
        std::shared_ptr<rhi::RHITextureView> view;
        rhi::SamplerDesc sampler;
    };
    std::unordered_map<std::string, Texture> textures;
    std::shared_ptr<const Core::Assets::SpriteSortingLayers> sortingLayers;
    std::shared_ptr<const SpriteShaderSnapshot> program;
    std::shared_ptr<Core::ResourceManagement::ResourceHandle<Render::Resources::Material>> materialOwner;
    bool materialAttempted = false;
    std::string materialError;
    bool available = Core::ServiceLocator::Contains<Assets::RuntimeAssetDatabase>() &&
        Core::ServiceLocator::Contains<Core::ResourceManagement::TextureManager>() &&
        Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>() &&
        Core::ServiceLocator::Contains<Core::ResourceManagement::ShaderManager>() &&
        Core::ServiceLocator::Contains<Core::ResourceManagement::ResourceLifetimeRegistry>();

    bool CaptureMaterial(std::string& error)
    {
        if (!materialAttempted)
        {
            materialAttempted = true;
            std::string path;
            auto* material = ResolveDefaultSpriteMaterial(path, materialError);
            if (material && (program = CaptureProgram(*material, materialError)))
            {
                auto& registry = NLS_SERVICE(Core::ResourceManagement::ResourceLifetimeRegistry);
                auto& manager = NLS_SERVICE(Core::ResourceManagement::MaterialManager);
                materialOwner = CaptureSharedResourceOwner(registry, manager, *material, path, [&](const std::string& token)
                {
                    return manager.AcquireRegisteredMaterialHandle(registry, token, path,
                        Core::ResourceManagement::ResourceLifetimeOwnerKind::RenderSnapshot);
                });
                if (!materialOwner) materialError = "Sprite material ownership could not be retained";
            }
        }
        if (program && materialOwner) return true;
        error = materialError;
        return false;
    }
};

std::shared_ptr<const SpriteRenderProxy> CaptureSprite(
    Components::SpriteRendererComponent& component, SpriteCaptureResources& resources, std::string& error)
{
    auto* owner = component.gameobject();
    if (!owner || !owner->IsAlive() || !owner->IsActive() || !component.IsSelfEnabled()) return {};
    auto sprite = component.CaptureSprite(resources.sprites);
    if (!sprite) { error = component.LastError(); return {}; }
    if (!resources.available)
    { error = "Sprite resource services are not initialized"; return {}; }
    const auto* entry = NLS_SERVICE(Assets::RuntimeAssetDatabase).Resolve(sprite->textureAssetId, sprite->textureSubAssetKey);
    if (!entry) { error = "Sprite source texture is missing from the runtime manifest"; return {}; }
    auto captured = std::make_shared<SpriteRenderProxy>(); captured->sprite = std::move(sprite);
    auto found = resources.textures.find(entry->artifactPath);
    if (found == resources.textures.end())
    {
        auto& textures = NLS_SERVICE(Core::ResourceManagement::TextureManager);
        auto* texture = textures.GetArtifactResource(entry->artifactPath);
        if (!texture) { error = "Sprite texture is unavailable"; return {}; }
        auto& registry = NLS_SERVICE(Core::ResourceManagement::ResourceLifetimeRegistry);
        SpriteCaptureResources::Texture value;
        value.owner = CaptureSharedResourceOwner(registry, textures, *texture, entry->artifactPath, [&](const std::string& token)
        {
            return textures.AcquireRegisteredTextureHandle(registry, token, entry->artifactPath,
                Core::ResourceManagement::ResourceLifetimeOwnerKind::RenderSnapshot);
        });
        if (!value.owner) { error = "Sprite texture ownership could not be retained"; return {}; }
        value.view = texture->GetOrCreateExplicitTextureView("SpriteTexture");
        if (!value.view) { error = "Sprite texture view is unavailable"; return {}; }
        value.sampler.minFilter = texture->firstFilter == Render::Settings::ETextureFilteringMode::NEAREST ? rhi::TextureFilter::Nearest : rhi::TextureFilter::Linear;
        value.sampler.magFilter = texture->secondFilter == Render::Settings::ETextureFilteringMode::NEAREST ? rhi::TextureFilter::Nearest : rhi::TextureFilter::Linear;
        value.sampler.wrapU = value.sampler.wrapV = value.sampler.wrapW = rhi::TextureWrap::ClampToEdge;
        found = resources.textures.emplace(entry->artifactPath, std::move(value)).first;
    }
    captured->textureOwner = found->second.owner;
    captured->texture = found->second.view;
    captured->sampler = found->second.sampler;
    if (!resources.CaptureMaterial(error)) return {};
    captured->program = resources.program;
    captured->materialOwner = resources.materialOwner;
    captured->world = owner->GetTransform()->GetWorldMatrix(); captured->ownerId = uint64_t(owner->GetInstanceID());
    if (const auto* scene = owner->GetScene(); scene && scene->IsPlaying() && scene->GetPhysicsSettings().interpolate)
        captured->world = scene->CapturePhysicsPresentationTransform(*owner).worldMatrix;
    captured->color = component.GetColor(); captured->flipX = component.GetFlipX(); captured->flipY = component.GetFlipY();
    captured->sliced=component.GetDrawMode()==Components::SpriteRendererComponent::ESpriteDrawMode::Sliced;
    captured->tiled=component.GetDrawMode()==Components::SpriteRendererComponent::ESpriteDrawMode::Tiled;
    captured->size=component.GetSize();
    captured->sortingLayer = component.GetSortingLayer(); captured->orderInLayer = component.GetOrderInLayer();
    if (!resources.sortingLayers) resources.sortingLayers = Assets::CaptureSpriteSortingLayers();
    captured->layerOrder = resources.sortingLayers->ResolveRank(captured->sortingLayer);
    captured->visibilityLayer = uint32_t(owner->GetLayer());
    error.clear(); return captured;
}
}

std::shared_ptr<const SpriteRenderProxy> CaptureSpriteRenderProxy(Components::SpriteRendererComponent& component, std::string& error)
{
    SpriteCaptureResources resources;
    return CaptureSprite(component, resources, error);
}

std::vector<std::shared_ptr<const SpriteRenderProxy>> CaptureSpriteRenderProxies(
    const std::vector<Components::SpriteRendererComponent*>& components)
{
    std::vector<std::shared_ptr<const SpriteRenderProxy>> result(components.size());
    if (components.empty()) return result;
    SpriteCaptureResources resources;
    for (size_t index = 0; index < components.size(); ++index)
        if (components[index])
        {
            std::string error;
            result[index] = CaptureSprite(*components[index], resources, error);
        }
    return result;
}

struct SpriteSceneRenderer::State
{
    struct Pipeline
    {
        uint64_t device = 0;
        bool picking = false;
        std::shared_ptr<const SpriteShaderSnapshot> program;
        rhi::RHIRenderTargetLayoutDesc targets;
        std::shared_ptr<rhi::RHIBindingLayout> passLayout, materialLayout;
        std::shared_ptr<rhi::RHIGraphicsPipeline> pipeline;
    };
    std::vector<Pipeline> pipelines;
    // libc++ has no std::atomic<std::shared_ptr<T>> specialisation, so the
    // published snapshot is guarded by a mutex. Loads and stores happen a few
    // times per frame, where the lock is not measurable.
    mutable std::mutex publishedMutex;
    std::shared_ptr<const Statistics> published {std::make_shared<const Statistics>()};
    Render::Sprites::SpriteBatchResult stats;
    bool hadVisibleSprites = false;
};
SpriteSceneRenderer::SpriteSceneRenderer() : m_state(std::make_unique<State>()) {}
SpriteSceneRenderer::~SpriteSceneRenderer() = default;
SpriteSceneRenderer::Statistics SpriteSceneRenderer::GetStatistics() const
{
    std::lock_guard lock(m_state->publishedMutex);
    return *m_state->published;
}

bool SpriteSceneRenderer::AppendPass(std::shared_ptr<rhi::RHIDevice> device, context::RenderScenePackage& package,
    const Render::FrameGraph::FrozenFrameDescriptor& view, const std::vector<std::shared_ptr<const SpriteRenderProxy>>& sprites,
    std::string& error, bool cpuExpanded)
{
    NLS_PROFILE_NAMED_SCOPE("Sprite2D::BuildRenderPass");
    m_state->stats = {};
    struct Publication
    {
        State& state; bool cpuExpanded=false, prepared=false;
        uint32_t submitted=0,visible=0,culled=0;
        ~Publication()
        {
            const auto& source=state.stats;auto value=std::make_shared<Statistics>();
            value->submitted=submitted;value->culled=culled;value->visible=visible;
            value->batches=uint32_t(source.batches.size());value->drawCalls=prepared?value->batches:0;
            value->materialBreaks=source.materialBreaks;value->textureBreaks=source.textureBreaks;
            value->stateBreaks=source.stateBreaks;value->capacityBreaks=source.capacityBreaks;value->orderingBreaks=source.orderingBreaks;
            value->cpuExpanded=cpuExpanded;value->prepared=prepared;
            state.hadVisibleSprites=prepared && !source.sorted.empty();
            {
                std::lock_guard lock(state.publishedMutex);
                state.published=std::move(value);
            }
        }
    } publication{*m_state,cpuExpanded};
    const bool hasScenePass = std::any_of(package.passCommandInputs.begin(), package.passCommandInputs.end(), [](const auto& pass)
    {
        return pass.kind != context::RenderPassCommandKind::Helper && pass.kind != context::RenderPassCommandKind::Transparent &&
            pass.kind != context::RenderPassCommandKind::Compute && pass.kind != context::RenderPassCommandKind::UIOverlay &&
            pass.kind != context::RenderPassCommandKind::SwapchainOutput && (pass.drawCount || pass.clearColor || pass.clearDepth || pass.clearStencil);
    });
    const bool hasPartialOutput = std::any_of(package.passCommandInputs.begin(), package.passCommandInputs.end(), [](const auto& pass)
    {
        return (pass.kind == context::RenderPassCommandKind::Helper || pass.kind == context::RenderPassCommandKind::Transparent) && pass.usesColorAttachment;
    });
    // Full scene renderers retain their own attachment initialization. Supply
    // the missing clear only for helper/transparent-only views, including the
    // frame after their last visible Sprite disappears.
    if (device && !hasScenePass && (!sprites.empty() || m_state->hadVisibleSprites || hasPartialOutput)) EnsureSceneOutputClears(package, view);
    if (sprites.empty()) { publication.prepared=true;error.clear(); return true; }
    if (!device || !view.descriptor.camera) { error = "Sprite draw requires a device and camera"; return false; }
    const auto& camera = *view.descriptor.camera;
    const auto vp = camera.GetProjectionMatrix() * camera.GetViewMatrix();
    const auto gpuViewProjection = camera.GetGpuProjectionMatrix() * camera.GetViewMatrix();
    std::vector<PackedSprite> packed;std::vector<std::shared_ptr<const SpriteRenderProxy>> geometrySources;
    std::vector<Render::Sprites::SpriteBatchItem> items;
    packed.reserve(sprites.size());geometrySources.reserve(sprites.size());items.reserve(sprites.size());
    for (uint32_t i = 0; i < sprites.size(); ++i)
    {
        const auto& sprite = sprites[i]; if (!sprite || !sprite->program || !sprite->sprite || !sprite->texture) continue;
        ++publication.submitted;
        std::optional<Core::Assets::SpritePatchGeometry> geometry;
        std::optional<Core::Assets::SpriteTiledGeometry> tiled;
        if(sprite->tiled)
        {
            tiled=Core::Assets::BuildTiledSpriteGeometry(*sprite->sprite,sprite->size.x,sprite->size.y,error);
            if(!tiled)return false;
        }
        else if(sprite->sliced)
        {
            geometry=Core::Assets::BuildSlicedSpriteGeometry(*sprite->sprite,sprite->size.x,sprite->size.y,error);
            if(!geometry)return false;
        }
        const auto bounds=tiled?tiled->bounds:geometry?geometry->bounds:sprite->sprite->LocalRect();
        const auto uv=sprite->sprite->UvRect();
        const auto boundsPacked=Pack(*sprite,bounds,uv);
        if(!camera.IsLayerVisible(int(sprite->visibilityLayer))||
            (camera.HasFrustumGeometryCulling()&&!Visible(boundsPacked,vp)))
        {++publication.culled;continue;}
        const auto center = (boundsPacked[0]+boundsPacked[2])*0.5f;
        const auto projected = camera.GetViewMatrix() * center;
        if(!std::isfinite(projected.z)){++publication.culled;continue;}
        ++publication.visible;
        for(uint32_t patch=0;patch<(tiled?uint32_t(tiled->patches.size()):geometry?geometry->count:1);++patch)
        {
            const auto index=uint32_t(packed.size());
            const auto* shape=tiled?&tiled->patches[patch]:geometry?&geometry->patches[patch]:nullptr;
            packed.push_back(shape?Pack(*sprite,shape->localRect,shape->uvRect):boundsPacked);
            geometrySources.push_back(sprite);
            items.push_back({{sprite->layerOrder,sprite->orderInLayer,-projected.z,sprite->ownerId},
                {sprite->program->identity,uint64_t(reinterpret_cast<uintptr_t>(sprite->texture.get())),
                    uint64_t(sprite->sampler.minFilter)|(uint64_t(sprite->sampler.magFilter)<<8)|(uint64_t(sprite->picking)<<16)},index,true});
        }
    }
    std::vector<Render::Sprites::SpriteSortKey> externalDraws;
    for (const auto& pass : package.passCommandInputs)
        if (pass.kind == context::RenderPassCommandKind::Transparent)
            for (const auto& draw : pass.recordedDrawCommands) if (draw.transparentSortKey) externalDraws.push_back(*draw.transparentSortKey);
    m_state->stats = Render::Sprites::BuildSpriteBatches(items,16384,externalDraws);
    if (m_state->stats.sorted.empty()) { publication.prepared=true;error.clear(); return true; }
    auto color = package.externalSceneOutputColorView; auto depth = package.externalSceneOutputDepthView;
    size_t insertion = package.passCommandInputs.size();
    bool colorBefore = false, depthBefore = false;
    for (size_t i = 0; i < package.passCommandInputs.size(); ++i)
    {
        const auto& previous = package.passCommandInputs[i];
        if (previous.kind == context::RenderPassCommandKind::UIOverlay || previous.kind == context::RenderPassCommandKind::SwapchainOutput)
        { insertion = i; break; }
        if (previous.kind == context::RenderPassCommandKind::Transparent && !previous.colorAttachmentViews.empty()) color = previous.colorAttachmentViews.front();
        if (previous.depthStencilAttachmentView) depth = previous.depthStencilAttachmentView;
        colorBefore |= (previous.drawCount || previous.clearColor) && previous.usesColorAttachment && previous.kind != context::RenderPassCommandKind::GBuffer;
        depthBefore |= (previous.drawCount || previous.clearDepth) && previous.writesDepthStencilAttachment;
    }
    rhi::RHIRenderTargetLayoutDesc targets;
    targets.colorFormats = {color ? color->GetDesc().format : rhi::TextureFormat::RGBA8};
    targets.colorSpaces = {color ? color->GetDesc().colorSpace : rhi::TextureColorSpace::Linear};
    targets.hasDepth = depth != nullptr; if (depth) targets.depthFormat = depth->GetDesc().format;
    context::RenderPassCommandInput pass;
    pass.kind = context::RenderPassCommandKind::Transparent; pass.debugName = "Sprite2DUnlit";
    pass.renderWidth = package.renderWidth; pass.renderHeight = package.renderHeight;
    pass.targetsSwapchain = package.targetsSwapchain && !color;
    pass.usesColorAttachment = true; pass.usesDepthStencilAttachment = targets.hasDepth;
    pass.clearColor = package.clearColorBuffer && !colorBefore; pass.clearColorValue = package.clearColorValue;
    pass.clearDepth = package.clearDepthBuffer && targets.hasDepth && !depthBefore; pass.writesDepthStencilAttachment = pass.clearDepth;
    pass.requiresDependencyVisibility = !package.passCommandInputs.empty();
    if (color) pass.colorAttachmentViews = {color}; pass.depthStencilAttachmentView = depth;
    pass.textureResourceAccesses.push_back({color ? color->GetTexture() : nullptr, color ? color->GetDesc().subresourceRange : rhi::RHISubresourceRange{},
        context::ResourceAccessMode::Write,rhi::ResourceState::RenderTarget,rhi::PipelineStageMask::RenderTarget,rhi::AccessMask::ColorAttachmentRead|rhi::AccessMask::ColorAttachmentWrite});
    if (depth) pass.textureResourceAccesses.push_back({depth->GetTexture(),depth->GetDesc().subresourceRange,
        pass.clearDepth ? context::ResourceAccessMode::Write : context::ResourceAccessMode::Read,
        pass.clearDepth ? rhi::ResourceState::DepthWrite : rhi::ResourceState::DepthRead,rhi::PipelineStageMask::DepthStencil,
        pass.clearDepth ? rhi::AccessMask::DepthStencilWrite : rhi::AccessMask::DepthStencilRead});
    std::vector<Maths::Vector4> data;
    constexpr uint32_t corners[] {0,1,2,0,2,3};
    for (const auto& sorted : m_state->stats.sorted)
    {
        const auto& value = packed[sorted.sourceIndex];
        if (!cpuExpanded) data.insert(data.end(), value.begin(), value.end());
        else for (const auto corner : corners)
        {
            data.push_back(value[corner]);
            data.push_back({corner == 0 || corner == 1 ? value[4].x : value[4].z,
                corner == 0 || corner == 3 ? value[4].w : value[4].y,0,0});
            data.push_back(value[5]);
        }
    }
    rhi::RHIBufferDesc bufferDesc; bufferDesc.size = data.size()*sizeof(Maths::Vector4);
    bufferDesc.usage = rhi::BufferUsageFlags::ShaderRead; bufferDesc.memoryUsage = rhi::MemoryUsage::CPUToGPU;
    bufferDesc.debugName = cpuExpanded ? "SpriteCpuBatchVertices" : "SpriteInstances";
    rhi::RHIBufferUploadDesc upload; upload.data = data.data(); upload.dataSize = bufferDesc.size;
    auto buffer = device->CreateBuffer(bufferDesc, upload);
    if (!buffer) { error = "Cannot allocate shared sprite data"; return false; }
    pass.bufferResourceAccesses.push_back({buffer,context::ResourceAccessMode::Read,rhi::ResourceState::GenericRead,rhi::PipelineStageMask::VertexShader,rhi::AccessMask::ShaderRead});
    for (const auto& batch : m_state->stats.batches)
    {
        const auto& first = geometrySources[m_state->stats.sorted[batch.first].sourceIndex];
        auto pipeline = std::find_if(m_state->pipelines.begin(),m_state->pipelines.end(),[&](const auto& entry)
        { return entry.device == device->GetCacheIdentity() && entry.picking == first->picking && entry.program == first->program && entry.targets.colorFormats == targets.colorFormats &&
            entry.targets.colorSpaces == targets.colorSpaces && entry.targets.hasDepth == targets.hasDepth && entry.targets.depthFormat == targets.depthFormat; });
        if (pipeline == m_state->pipelines.end())
        {
            State::Pipeline entry; entry.device = device->GetCacheIdentity(); entry.picking = first->picking; entry.program = first->program; entry.targets = targets;
            rhi::RHIBindingLayoutDesc layout; layout.debugName = "SpritePassLayout";
            layout.entries = {
                {"SpriteView",rhi::BindingType::UniformBuffer,rhi::BindingPointMap::kPassDescriptorSet,0,1,rhi::ShaderStageMask::AllGraphics,rhi::BindingPointMap::kPassBindingSpace,0},
                {"SpriteData",rhi::BindingType::StructuredBuffer,rhi::BindingPointMap::kPassDescriptorSet,0,1,rhi::ShaderStageMask::Vertex,rhi::BindingPointMap::kPassBindingSpace,16},
                {"SpriteTexture",rhi::BindingType::Texture,rhi::BindingPointMap::kPassDescriptorSet,1,1,rhi::ShaderStageMask::Fragment,rhi::BindingPointMap::kPassBindingSpace,0},
                {"SpriteSampler",rhi::BindingType::Sampler,rhi::BindingPointMap::kPassDescriptorSet,0,1,rhi::ShaderStageMask::Fragment,rhi::BindingPointMap::kPassBindingSpace,0}};
            entry.passLayout = device->CreateBindingLayout(layout);
            entry.materialLayout = device->CreateBindingLayout(first->program->materialLayout);
            rhi::RHIPipelineLayoutDesc pipelineLayout;
            pipelineLayout.bindingLayouts.resize((std::max)(rhi::BindingPointMap::kPassDescriptorSet,rhi::BindingPointMap::kMaterialDescriptorSet)+1);
            pipelineLayout.bindingLayouts[rhi::BindingPointMap::kPassDescriptorSet] = entry.passLayout;
            pipelineLayout.bindingLayouts[rhi::BindingPointMap::kMaterialDescriptorSet] = entry.materialLayout;
            rhi::RHIGraphicsPipelineDesc desc; desc.pipelineLayout = device->CreatePipelineLayout(pipelineLayout);
            for (const auto& module : first->program->shaders)
                if (module.targetBackend == device->GetAdapter()->GetBackendType())
                {
                    if (module.stage == rhi::ShaderStage::Vertex) desc.vertexShader = device->CreateShaderModule(module);
                    else desc.fragmentShader = device->CreateShaderModule(module);
                }
            desc.renderTargetLayout = targets; desc.rasterState.cullEnabled = false;
            desc.depthStencilState.depthTest = targets.hasDepth; desc.depthStencilState.depthWrite = false;
            desc.depthStencilState.depthCompare = Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
            desc.blendState.enabled = !first->picking; rhi::RHIRenderTargetBlendStateDesc blend; blend.blendEnable = !first->picking;
            desc.blendState.renderTargets = {blend}; desc.debugName = "Sprite2DUnlit";
            if (!entry.passLayout || !entry.materialLayout || !desc.pipelineLayout || !desc.vertexShader || !desc.fragmentShader ||
                !(entry.pipeline = device->CreateGraphicsPipeline(desc))) { error = "Cannot create Sprite pipeline for this target"; return false; }
            if (m_state->pipelines.size() >= 64) m_state->pipelines.erase(m_state->pipelines.begin());
            m_state->pipelines.push_back(std::move(entry)); pipeline = std::prev(m_state->pipelines.end());
        }
        struct DrawOwner
        {
            std::vector<std::shared_ptr<const SpriteRenderProxy>> sprites;
            std::shared_ptr<rhi::RHIBuffer> data, view;
            std::vector<std::shared_ptr<rhi::RHIBuffer>> material;
            std::shared_ptr<rhi::RHISampler> sampler;
        };
        auto owner = std::make_shared<DrawOwner>(); owner->data = buffer;
        for (uint32_t i = batch.first; i < batch.first+batch.count; ++i) owner->sprites.push_back(geometrySources[m_state->stats.sorted[i].sourceIndex]);
        struct Constants { Maths::Matrix4 vp; Maths::Vector4 mode; } constants {gpuViewProjection,{cpuExpanded ? 1.f : 0.f,float(batch.first),first->picking ? 1.f : 0.f,0}};
        rhi::RHIBufferDesc desc; desc.size = sizeof(constants); desc.usage = rhi::BufferUsageFlags::Uniform; desc.memoryUsage = rhi::MemoryUsage::CPUToGPU;
        rhi::RHIBufferUploadDesc viewUpload; viewUpload.data = &constants; viewUpload.dataSize = sizeof(constants);
        owner->view = device->CreateBuffer(desc,viewUpload);
        owner->sampler = device->CreateSampler(first->sampler);
        rhi::RHIBindingSetDesc bindings; bindings.layout = pipeline->passLayout;
        rhi::RHIBindingSetEntry cb, values, texture, sampler;
        cb.type = rhi::BindingType::UniformBuffer; cb.buffer = owner->view; cb.bufferRange = sizeof(constants);
        values.type = rhi::BindingType::StructuredBuffer; values.buffer = buffer; values.bufferRange = buffer->GetDesc().size; values.elementStride = 16;
        texture.binding = 1; texture.type = rhi::BindingType::Texture; texture.textureView = first->texture;
        sampler.type = rhi::BindingType::Sampler; sampler.sampler = owner->sampler;
        bindings.entries = {cb,values,texture,sampler};
        context::RecordedDrawCommandInput command; command.pipeline = pipeline->pipeline; command.passBindingSet = device->CreateBindingSet(bindings);
        rhi::RHIBindingSetDesc materialBindings; materialBindings.layout = pipeline->materialLayout;
        for (const auto& constant : first->program->constants)
        {
            desc.size = constant.bytes.size(); upload.data = constant.bytes.data(); upload.dataSize = desc.size;
            auto materialBuffer = device->CreateBuffer(desc,upload); owner->material.push_back(materialBuffer);
            rhi::RHIBindingSetEntry binding; binding.binding = constant.binding; binding.type = rhi::BindingType::UniformBuffer;
            binding.buffer = materialBuffer; binding.bufferRange = desc.size; materialBindings.entries.push_back(binding);
        }
        command.materialBindingSet = device->CreateBindingSet(materialBindings);
        if (!owner->view || !owner->sampler || !command.passBindingSet || !command.materialBindingSet)
        { error = "Cannot bind sprite draw resources"; return false; }
        command.instanceCount = cpuExpanded ? 1 : batch.count;
        command.vertexStart = 0; command.vertexCount = cpuExpanded ? batch.count*6 : 6;
        command.transparentSortKey = m_state->stats.sorted[batch.first].sorting;
        command.resourceOwner = owner; pass.recordedDrawCommands.push_back(std::move(command));
        pass.textureResourceAccesses.push_back({first->texture->GetTexture(),first->texture->GetDesc().subresourceRange,
            context::ResourceAccessMode::Read,rhi::ResourceState::ShaderRead,rhi::PipelineStageMask::FragmentShader,rhi::AccessMask::ShaderRead});
    }
    pass.drawCount = pass.recordedDrawCommands.size();
    package.visibleDrawCount += pass.drawCount; package.drawCommandCount += pass.drawCount;
    package.transparentDrawCount += pass.drawCount; package.materialBatchCount += pass.drawCount;
    package.hasTransparentPass = true; package.hasVisibleDraws = true; package.containsCommandInputs = true;
    InsertPass(package,std::move(pass),insertion);
    publication.prepared=true;
    error.clear(); return true;
}
}
