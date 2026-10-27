#include "ParticleRenderer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Geometry/Vertex.h>

namespace NLS::VFX
{
bool ParticleRenderer::Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const std::vector<Attribute>& attributes,
    const Render::RHI::RHIShaderModuleDesc& vertex, const Render::RHI::RHIShaderModuleDesc& fragment,
    const Render::RHI::RHIRenderTargetLayoutDesc& targets, std::string& error, const GpuSortShaders* sorting,
    const GpuVisibilityShaders* visibility, const GpuBoundsShaders* bounds, const SpriteSettings& settings, const MeshSettings* mesh)
{
    using namespace Render::RHI;
    if (!device || !device->IsBackendReady() || !device->GetCapabilities().SupportsFeature(RHIDeviceFeature::DrawIndirect) ||
        vertex.stage != ShaderStage::Vertex || fragment.stage != ShaderStage::Fragment || targets.colorFormats.size() != 1)
    { error = "Sprite renderer requires indirect draw support, vertex/fragment shaders and one color target"; return false; }
    if (!ValidateSpriteSettings(settings,error)) return false;
    if (mesh && (!visibility || !bounds || vertex.entryPoint != "MeshVS" || mesh->meshes.empty() || !ValidateMeshSettings(*mesh,error)))
    { if (error.empty()) error = "Mesh renderer requires mesh shaders, bounds, at least one mesh slot and per-view indirect visibility"; return false; }
    RHIBindingLayoutDesc binding;
    binding.debugName = "VfxSpriteBindings";
    binding.entries = {
        {"VfxSpriteView",BindingType::UniformBuffer,BindingPointMap::kPassDescriptorSet,0,1,ShaderStageMask::AllGraphics,BindingPointMap::kPassBindingSpace,0},
        {"VfxAttributes",BindingType::StructuredBuffer,BindingPointMap::kPassDescriptorSet,0,1,ShaderStageMask::Vertex,BindingPointMap::kPassBindingSpace,sizeof(float)},
        {"VfxSortKeys",BindingType::StructuredBuffer,BindingPointMap::kPassDescriptorSet,1,1,ShaderStageMask::Vertex,BindingPointMap::kPassBindingSpace,16},
        {"VfxSpriteTexture",BindingType::Texture,BindingPointMap::kPassDescriptorSet,2,1,ShaderStageMask::Fragment,BindingPointMap::kPassBindingSpace,0},
        {"VfxSceneDepth",BindingType::Texture,BindingPointMap::kPassDescriptorSet,3,1,ShaderStageMask::Fragment,BindingPointMap::kPassBindingSpace,0},
        {"VfxSpriteSampler",BindingType::Sampler,BindingPointMap::kPassDescriptorSet,0,1,ShaderStageMask::Fragment,BindingPointMap::kPassBindingSpace,0}};
    auto layout = device->CreateBindingLayout(binding);
    if (!layout) { error = "Sprite binding layout creation failed"; return false; }
    RHIPipelineLayoutDesc pipelineLayout;
    pipelineLayout.bindingLayouts.resize(BindingPointMap::kPassDescriptorSet+1);
    pipelineLayout.bindingLayouts[BindingPointMap::kPassDescriptorSet] = layout; pipelineLayout.debugName = "VfxSpriteLayout";
    RHIGraphicsPipelineDesc pipeline;
    pipeline.pipelineLayout = device->CreatePipelineLayout(pipelineLayout);
    pipeline.vertexShader = device->CreateShaderModule(vertex); pipeline.fragmentShader = device->CreateShaderModule(fragment);
    pipeline.renderTargetLayout = targets; pipeline.debugName = "VfxSprites";
    pipeline.rasterState.cullEnabled = false;
    if (mesh)
    {
        pipeline.vertexBuffers = {{0,sizeof(Render::Geometry::Vertex),false}};
        pipeline.vertexAttributes = {
            {0,0,offsetof(Render::Geometry::Vertex,position),12,RHIVertexSemantic::Position,0},
            {1,0,offsetof(Render::Geometry::Vertex,texCoords),8,RHIVertexSemantic::TexCoord,0}};
        pipeline.debugName = "VfxMeshes";
    }
    const bool writesDepth = settings.blend == SpriteBlendMode::Masked || settings.blend == SpriteBlendMode::Opaque;
    pipeline.depthStencilState.depthTest = targets.hasDepth;
    pipeline.depthStencilState.depthWrite = targets.hasDepth && writesDepth;
    RHIRenderTargetBlendStateDesc blend;
    blend.blendEnable = !writesDepth;
    if (settings.blend == SpriteBlendMode::Additive) { blend.dstColor = RHIBlendFactor::One; blend.dstAlpha = RHIBlendFactor::One; }
    if (settings.blend == SpriteBlendMode::Premultiplied) blend.srcColor = RHIBlendFactor::One;
    pipeline.blendState.enabled = blend.blendEnable; pipeline.blendState.renderTargets = {blend};
    if (!pipeline.pipelineLayout || !pipeline.vertexShader || !pipeline.fragmentShader)
    { error = "Sprite shader or pipeline layout creation failed"; return false; }
    auto graphics = device->CreateGraphicsPipeline(pipeline);
    if (!graphics) { error = "Sprite graphics pipeline creation failed"; return false; }
    std::shared_ptr<ParticleSorter> sorter;
    if (sorting)
    {
        sorter = std::make_shared<ParticleSorter>();
        if (!sorter->Initialize(device,attributes,*sorting,error)) return false;
    }
    std::shared_ptr<ParticleBoundsCalculator> calculator;
    std::shared_ptr<ParticleVisibility> culler;
    if (visibility)
    {
        culler = std::make_shared<ParticleVisibility>();
        if (!culler->Initialize(device,attributes,*visibility,error,mesh)) return false;
        if (bounds)
        {
            calculator = std::make_shared<ParticleBoundsCalculator>();
            if (!calculator->Initialize(device,attributes,*bounds,error,mesh)) return false;
        }
    }
    const std::array<uint32_t,4> identity{};
    RHIBufferDesc desc; desc.size = sizeof(identity); desc.usage = BufferUsageFlags::ShaderRead; desc.memoryUsage = MemoryUsage::CPUToGPU;
    desc.debugName = "VfxUnsortedKey";
    RHIBufferUploadDesc upload; upload.data = identity.data(); upload.dataSize = sizeof(identity);
    auto unsorted = device->CreateBuffer(desc,upload);
    if (!unsorted) { error = "Sprite default index allocation failed"; return false; }
    const auto defaultTexture = [&](TextureFormat format, const void* data, const char* name)
    {
        RHITextureDesc desc; desc.extent = {1,1,1}; desc.format = format; desc.debugName = name;
        RHITextureUploadDesc upload; upload.data = data; upload.dataSize = 4; upload.rowPitch = 4; upload.slicePitch = 4;
        auto texture = device->CreateTexture(desc,upload);
        RHITextureViewDesc view; view.format = format;
        return texture ? device->CreateTextureView(texture,view) : nullptr;
    };
    const uint32_t white = 0xffffffff; const float farDepth = 1;
    auto whiteView = defaultTexture(TextureFormat::RGBA8,&white,"VfxWhite");
    auto depthView = defaultTexture(TextureFormat::R32F,&farDepth,"VfxFarDepth");
    if (!whiteView || !depthView) { error = "Sprite fallback texture allocation failed"; return false; }
    m_device = std::move(device); m_layout = std::move(layout); m_pipeline = std::move(graphics); m_attributes = attributes;
    m_sorter = std::move(sorter); m_unsortedKey = std::move(unsorted);
    m_bounds = std::move(calculator); m_visibility = std::move(culler);
    m_white = std::move(whiteView); m_farDepth = std::move(depthView); m_settings = settings;
    m_meshRenderer = mesh != nullptr;
    m_meshSlotCount = mesh ? static_cast<uint32_t>(mesh->meshes.size()) : 0u;
    m_meshLodIndex = mesh ? mesh->lodIndex : -1;
    m_materialPipelines.clear();
    error.clear(); return true;
}

std::shared_ptr<const SpriteDrawResources> ParticleRenderer::Record(Render::RHI::RHICommandBuffer& commands,
    const GpuDataSet& data, const SpriteView& view, const Render::RHI::RHIRenderPassDesc& pass, std::string& error,
    const SpriteDrawContext& context) const
{
    ParticleRenderData renderData;
    renderData.attributes = data.Attributes(); renderData.capacity = data.Capacity(); renderData.countUpperBound = data.CountUpperBound();
    renderData.values = data.AttributeBuffer(); renderData.ids = data.IdBuffer(); renderData.counters = data.CounterBuffer();
    renderData.bounds = data.BoundsBuffer();
    return Record(commands,renderData,view,pass,error,context);
}

std::shared_ptr<const SpriteDrawResources> ParticleRenderer::Record(Render::RHI::RHICommandBuffer& commands,
    const ParticleRenderData& renderData, const SpriteView& view, const Render::RHI::RHIRenderPassDesc& pass, std::string& error,
    const SpriteDrawContext& context) const
{
    using namespace Render::RHI;
    if (!m_device || !m_pipeline || !commands.IsRecording() || commands.IsChildCommandBuffer() ||
        pass.colorAttachments.size() != 1 || !pass.colorAttachments[0].view || !pass.renderArea.width || !pass.renderArea.height)
    { error = "Invalid Sprite draw, view, data schema or output pass"; return {}; }
    const auto& targets = m_pipeline->GetDesc().renderTargetLayout;
    if (pass.colorAttachments[0].view->GetDesc().format != targets.colorFormats[0] ||
        pass.depthStencilAttachment.has_value() != targets.hasDepth ||
        (targets.hasDepth && (!pass.depthStencilAttachment->view || pass.depthStencilAttachment->view->GetDesc().format != targets.depthFormat)))
    { error = "Sprite pass attachment layout differs from its graphics pipeline"; return {}; }
    if (targets.hasDepth && WritesDepth() && pass.depthStencilAttachment->readOnlyDepthStencil)
    { error = "Opaque and masked sprites require a writable depth attachment"; return {}; }
    auto drawContext = context;
    if (context.sceneDepth && pass.depthStencilAttachment && context.sceneDepth->GetTexture() == pass.depthStencilAttachment->view->GetTexture())
        drawContext.sceneDepthState = ResourceState::DepthRead|ResourceState::ShaderRead;
    auto resources = Prepare(renderData,view,error,drawContext);
    if (!resources) return {};
    try
    {
        if (Render::Context::RecordComputeDispatchInputs(commands,resources->dispatches) != resources->dispatches.size())
        { error = "Sprite preparation dispatch recording failed"; return {}; }
    }
    catch (const std::exception& exception) { error = exception.what(); return {}; }
    RHIBarrierDesc barrier;
    barrier.bufferBarriers = {
        {resources->attributes,ResourceState::Unknown,resources->attributes->GetDesc().memoryUsage == MemoryUsage::CPUToGPU ? ResourceState::GenericRead : ResourceState::ShaderRead,PipelineStageMask::AllCommands,
            PipelineStageMask::VertexShader,AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::ShaderRead},
        {resources->arguments,ResourceState::Unknown,resources->arguments->GetDesc().memoryUsage == MemoryUsage::CPUToGPU ? ResourceState::GenericRead : ResourceState::IndirectArgument,PipelineStageMask::AllCommands,
            PipelineStageMask::DrawIndirect,AccessMask::MemoryRead|AccessMask::MemoryWrite,AccessMask::IndirectRead}};
    const auto textureBarrier = [&](const std::shared_ptr<RHITextureView>& texture, ResourceState state, PipelineStageMask stage, AccessMask access)
    {
        barrier.textureBarriers.push_back({texture->GetTexture(),texture->GetTexture()->GetState(),state,texture->GetDesc().subresourceRange,
            PipelineStageMask::AllCommands,stage,AccessMask::MemoryRead|AccessMask::MemoryWrite,access});
    };
    textureBarrier(resources->texture,ResourceState::ShaderRead,PipelineStageMask::FragmentShader,AccessMask::ShaderRead);
    for (const auto& slot : resources->meshSlots)
    {
        textureBarrier(slot.texture,ResourceState::ShaderRead,PipelineStageMask::FragmentShader,AccessMask::ShaderRead);
        for (const auto& texture : slot.material.textures)
            textureBarrier(texture,ResourceState::ShaderRead,PipelineStageMask::FragmentShader,AccessMask::ShaderRead);
        for (const auto& buffer : {slot.mesh->GetVertexBuffer(),slot.mesh->GetIndexBuffer()})
            barrier.bufferBarriers.push_back({buffer,ResourceState::Unknown,
                buffer->GetDesc().memoryUsage == MemoryUsage::CPUToGPU ? ResourceState::GenericRead : ResourceState::VertexBuffer|ResourceState::IndexBuffer,
                PipelineStageMask::AllCommands,PipelineStageMask::VertexInput,AccessMask::MemoryRead|AccessMask::MemoryWrite,
                AccessMask::VertexRead|AccessMask::IndexRead});
    }
    for (const auto& texture : resources->materialTextures)
        textureBarrier(texture,ResourceState::ShaderRead,PipelineStageMask::FragmentShader,AccessMask::ShaderRead);
    const bool sharedDepth = pass.depthStencilAttachment && resources->sceneDepth->GetTexture() == pass.depthStencilAttachment->view->GetTexture();
    if (sharedDepth && (!pass.depthStencilAttachment->readOnlyDepthStencil || pass.depthStencilAttachment->depthLoadOp != LoadOp::Load))
    { error = "Soft particles require a loaded, read-only depth attachment"; return {}; }
    textureBarrier(resources->sceneDepth,sharedDepth ? ResourceState::DepthRead|ResourceState::ShaderRead : ResourceState::ShaderRead,
        sharedDepth ? PipelineStageMask::DepthStencil|PipelineStageMask::FragmentShader : PipelineStageMask::FragmentShader,
        sharedDepth ? AccessMask::DepthStencilRead|AccessMask::ShaderRead : AccessMask::ShaderRead);
    textureBarrier(pass.colorAttachments[0].view,ResourceState::RenderTarget,PipelineStageMask::RenderTarget,AccessMask::ColorAttachmentWrite);
    if (pass.depthStencilAttachment && !sharedDepth)
        textureBarrier(pass.depthStencilAttachment->view,pass.depthStencilAttachment->readOnlyDepthStencil ? ResourceState::DepthRead : ResourceState::DepthWrite,
            PipelineStageMask::DepthStencil,pass.depthStencilAttachment->readOnlyDepthStencil ? AccessMask::DepthStencilRead : AccessMask::DepthStencilWrite);
    auto recorded = commands.BarrierChecked(barrier);
    if (!recorded.Succeeded()) { error = recorded.message; return {}; }
    auto externalPass = pass; externalPass.attachmentsRequireExternalStateTransitions = true;
    commands.BeginRenderPass(externalPass);
    commands.SetViewport({static_cast<float>(pass.renderArea.x),static_cast<float>(pass.renderArea.y),
        static_cast<float>(pass.renderArea.width),static_cast<float>(pass.renderArea.height),0,1});
    commands.SetScissor(pass.renderArea);
    if (resources->meshSlots.empty())
    {
        commands.BindGraphicsPipeline(resources->pipeline);
        commands.BindBindingSet(BindingPointMap::kPassDescriptorSet,resources->bindings);
        if (resources->materialBindings) commands.BindBindingSet(BindingPointMap::kMaterialDescriptorSet,resources->materialBindings);
        recorded = commands.DrawIndirect(resources->arguments,4*sizeof(uint32_t));
    }
    else
    {
        for (const auto& slot : resources->meshSlots)
        {
            const auto& mesh = slot.mesh;
            commands.BindGraphicsPipeline(slot.material.pipeline ? slot.material.pipeline : resources->pipeline);
            commands.BindBindingSet(BindingPointMap::kPassDescriptorSet,slot.bindings ? slot.bindings : resources->bindings);
            if (slot.material.bindings) commands.BindBindingSet(BindingPointMap::kMaterialDescriptorSet,slot.material.bindings);
            commands.BindVertexBuffer(0,{mesh->GetVertexBuffer(),0,mesh->GetVertexStride()});
            commands.BindIndexBuffer({mesh->GetIndexBuffer(),0,mesh->GetIndexType()});
            recorded = commands.DrawIndexedIndirect(resources->arguments,slot.argumentOffset);
            if (!recorded.Succeeded()) break;
        }
    }
    commands.EndRenderPass();
    if (!recorded.Succeeded()) { error = recorded.message; return {}; }
    error.clear(); return resources;
}

std::shared_ptr<const SpriteDrawResources> ParticleRenderer::Prepare(const ParticleRenderData& data, const SpriteView& view, std::string& error,
    const SpriteDrawContext& context) const
{
    using namespace Render::RHI;
    const auto finite = [](const auto& values) { return std::all_of(values.begin(),values.end(),[](float value) { return std::isfinite(value); }); };
    const auto& material = m_meshRenderer && !context.meshMaterials.empty() ? context.meshMaterials.front() : context.material;
    std::shared_ptr<MeshDrawInfo> meshDrawInfo;
    std::vector<uint32_t> meshLods;
    if (!m_device || !m_pipeline || !data.values || !data.counters || !data.capacity || data.attributes.size() != m_attributes.size() ||
        !finite(view.viewProjection) || !finite(view.localToWorld) || !finite(view.cameraRight) || !finite(view.cameraUp) || !finite(view.cameraForward) ||
        !finite(material.tint) || !finite(material.uvTransform) || !finite(context.depthProjection) ||
        !std::isfinite(material.emissive) || material.emissive < 0)
    { error = "Invalid Sprite render data or view"; return {}; }
    if (m_meshRenderer)
    {
        if (context.meshes.empty() || context.meshes.size() != m_meshSlotCount)
        { error = "Mesh renderer slot count does not match the captured emitter"; return {}; }
        meshDrawInfo = std::make_shared<MeshDrawInfo>();
        meshDrawInfo->bounds.reserve(context.meshes.size()); meshDrawInfo->indexCounts.reserve(context.meshes.size());
        meshLods.reserve(context.meshes.size());
        for (const auto& geometry : context.meshes)
        {
            if (!geometry || geometry->levels.empty() || !finite(geometry->bounds) || geometry->bounds[3] < 0)
            { error = "Mesh renderer requires captured LOD levels and finite local bounds"; return {}; }
            const uint32_t lod = geometry->ChooseLod(view.viewProjection,view.localToWorld,view.cameraUp,m_meshLodIndex);
            const auto& mesh = geometry->levels[lod].mesh;
            if (!mesh || !mesh->GetVertexBuffer() || !mesh->GetIndexBuffer() || !mesh->GetIndexCount() || mesh->GetIndexCount()%3 ||
                !mesh->GetVertexCount() || mesh->GetVertexStride() != sizeof(Render::Geometry::Vertex))
            { error = "Mesh renderer requires frozen indexed triangle geometry"; return {}; }
            const auto& vertices = mesh->GetVertexBuffer()->GetDesc(); const auto& indices = mesh->GetIndexBuffer()->GetDesc();
            const uint32_t indexStride = mesh->GetIndexType() == IndexType::UInt16 ? 2u : 4u;
            if (!HasBufferUsage(vertices.usage,BufferUsageFlags::Vertex) || !HasBufferUsage(indices.usage,BufferUsageFlags::Index) ||
                vertices.size < uint64_t(mesh->GetVertexCount())*mesh->GetVertexStride() || indices.size < uint64_t(mesh->GetIndexCount())*indexStride)
            { error = "Particle mesh buffers do not match their declared geometry"; return {}; }
            meshLods.push_back(lod);
            meshDrawInfo->bounds.push_back(geometry->bounds);
            meshDrawInfo->indexCounts.push_back(mesh->GetIndexCount());
        }
    }
    for (size_t index = 0; index < m_attributes.size(); ++index)
        if (data.attributes[index].name != m_attributes[index].name || data.attributes[index].type != m_attributes[index].type)
        { error = "Sprite shader attribute layout mismatch"; return {}; }
    struct Constants
    {
        std::array<float,16> viewProjection, localToWorld;
        Value cameraRight, cameraUp;
        Value tint, uvTransform, depthProjection, shading;
        uint32_t capacity, sorted, alignment, blend;
        uint32_t subImagesX, subImagesY, blendSubImages, circularMask;
        std::array<Value,4> dynamicDefaults;
    };
    static_assert(sizeof(Constants) == Render::Resources::kParticleSpriteViewBytes);
    auto dynamicDefaults = material.dynamicDefaults;
    for (size_t i = 0; i < dynamicDefaults.size(); ++i)
    {
        if (!material.hasDynamicDefaults) dynamicDefaults[i] = m_settings.dynamicParameters[i].defaultValue;
        if (!finite(dynamicDefaults[i])) { error = "Dynamic material parameters must be finite"; return {}; }
    }
    auto tint = m_settings.tint;
    for (size_t i = 0; i < tint.size(); ++i) tint[i] *= material.tint[i];
    const bool usesDepth = m_settings.softDistance > 0 && context.sceneDepth;
    const Constants constants{view.viewProjection,view.localToWorld,view.cameraRight,view.cameraUp,tint,material.uvTransform,context.depthProjection,
        // Mesh reuses the unused VfxShading.w lane as the per-draw slot so the
        // public 320-byte material ABI stays unchanged.
        {m_settings.emissive*material.emissive,m_settings.alphaCutoff,usesDepth ? m_settings.softDistance : 0,0},
        data.capacity,(m_sorter || m_visibility) ? 1u : 0u,static_cast<uint32_t>(m_settings.alignment),static_cast<uint32_t>(m_settings.blend),
        m_settings.subImagesX,m_settings.subImagesY,m_settings.blendSubImages ? 1u : 0u,(!m_meshRenderer && m_settings.circularMask) ? 1u : 0u,dynamicDefaults};
    RHIBufferDesc desc;
    desc.size = sizeof(constants); desc.usage = BufferUsageFlags::Uniform; desc.memoryUsage = MemoryUsage::CPUToGPU; desc.debugName = "VfxSpriteView";
    RHIBufferUploadDesc upload; upload.data = &constants; upload.dataSize = sizeof(constants);
    auto resources = std::make_shared<SpriteDrawResources>();
    resources->device = m_device;
    resources->constants = m_device->CreateBuffer(desc,upload);
    resources->attributes = data.values; resources->arguments = data.counters; resources->pipeline = m_pipeline;
    const auto makeTexture = [&](const SpriteMaterial& value) -> std::shared_ptr<RHITextureView>
    {
        if (!value.texture) return m_white;
        const auto& texture = value.texture->GetDesc();
        if (texture.dimension != TextureDimension::Texture2D || texture.arrayLayers != 1 || texture.sampleCount != 1 ||
            !HasTextureUsage(texture.usage,TextureUsageFlags::Sampled))
        { error = "Particle sprite texture must be a sampled, single-layer 2D texture"; return {}; }
        RHITextureViewDesc desc; desc.format = texture.format; desc.colorSpace = texture.colorSpace;
        return m_device->CreateTextureView(value.texture,desc);
    };
    resources->texture = makeTexture(material);
    resources->sceneDepth = usesDepth ? context.sceneDepth : m_farDepth;
    resources->usesSceneDepth = usesDepth;
    resources->sampler = m_device->CreateSampler(material.sampler);
    if (!resources->texture || !resources->sceneDepth || !resources->sampler)
    { error = "Sprite texture, depth or sampler creation failed"; return {}; }
    auto visibleData = data;
    if (m_visibility)
    {
        if ((!visibleData.bounds || m_meshRenderer) && m_bounds)
        {
            resources->calculatedBounds = m_bounds->Prepare(data,error,m_meshRenderer ? meshDrawInfo->bounds : std::vector<Value>{});
            if (!resources->calculatedBounds) return {};
            visibleData.bounds = resources->calculatedBounds->bounds;
            resources->dispatches = resources->calculatedBounds->dispatches;
        }
        resources->visibility = m_visibility->Prepare(visibleData,view,error,meshDrawInfo.get());
        if (!resources->visibility) return {};
        resources->arguments = resources->visibility->arguments;
        resources->dispatches.insert(resources->dispatches.end(),resources->visibility->dispatches.begin(),resources->visibility->dispatches.end());
    }
    if (m_sorter)
    {
        resources->sorting = m_sorter->Prepare(data,view.localToWorld,view.cameraForward,error,resources->visibility ? resources->visibility->keys : nullptr);
        if (!resources->sorting) return {};
        resources->dispatches.insert(resources->dispatches.end(),resources->sorting->dispatches.begin(),resources->sorting->dispatches.end());
    }
    if (!resources->constants) { error = "Sprite view allocation failed"; return {}; }
    const auto makeBindings = [&](const std::shared_ptr<RHIBuffer>& viewConstants,
        const std::shared_ptr<RHITextureView>& textureView, const std::shared_ptr<RHISampler>& textureSampler)
    {
        RHIBindingSetDesc binding;
        binding.layout = m_layout; binding.debugName = "VfxSpriteViewBindings";
        RHIBindingSetEntry viewBinding;
        viewBinding.type = BindingType::UniformBuffer; viewBinding.buffer = viewConstants; viewBinding.bufferRange = sizeof(constants);
        RHIBindingSetEntry attributes;
        attributes.type = BindingType::StructuredBuffer; attributes.buffer = resources->attributes;
        attributes.bufferRange = attributes.buffer->GetDesc().size; attributes.elementStride = sizeof(float);
        RHIBindingSetEntry keys;
        keys.binding = 1; keys.type = BindingType::StructuredBuffer;
        keys.buffer = resources->sorting ? resources->sorting->keys : resources->visibility ? resources->visibility->keys : m_unsortedKey;
        keys.bufferRange = keys.buffer->GetDesc().size; keys.elementStride = 16;
        RHIBindingSetEntry texture, depth, sampler;
        texture.binding = 2; texture.type = BindingType::Texture; texture.textureView = textureView;
        depth.binding = 3; depth.type = BindingType::Texture; depth.textureView = resources->sceneDepth;
        if (usesDepth) depth.textureState = context.sceneDepthState;
        sampler.type = BindingType::Sampler; sampler.sampler = textureSampler;
        binding.entries = {std::move(viewBinding),std::move(attributes),std::move(keys),std::move(texture),std::move(depth),std::move(sampler)};
        return m_device->CreateBindingSet(binding);
    };
    resources->bindings = makeBindings(resources->constants,resources->texture,resources->sampler);
    if (!resources->bindings) { error = "Sprite view binding creation failed"; return {}; }
    ParticleDrawResources::MaterialBinding baseMaterial;
    if (!PrepareMaterial(baseMaterial,material,error)) return {};
    resources->pipeline = baseMaterial.pipeline ? baseMaterial.pipeline : m_pipeline;
    resources->materialBindings = baseMaterial.bindings;
    resources->materialTextures = baseMaterial.textures;
    resources->materialConstants = baseMaterial.constants;
    resources->materialSamplers = baseMaterial.samplers;
    if (m_meshRenderer)
    {
        resources->meshSlots.resize(context.meshes.size());
        for (std::size_t index = 0; index < resources->meshSlots.size(); ++index)
        {
            auto& slot = resources->meshSlots[index];
            slot.geometry = context.meshes[index];
            slot.mesh = slot.geometry->levels[meshLods[index]].mesh;
            slot.indexCount = slot.mesh->GetIndexCount();
            slot.argumentOffset = resources->meshSlots.size() > 1 ? 48u+20u*static_cast<uint64_t>(index) : 16u;
            slot.constants = resources->constants; slot.bindings = resources->bindings;
            slot.texture = resources->texture; slot.sampler = resources->sampler;
            const auto& slotMaterial = index < context.meshMaterials.size() ? context.meshMaterials[index] : context.material;
            if (index != 0)
            {
                auto slotConstants = constants; slotConstants.shading[3] = static_cast<float>(index);
                if (!finite(slotMaterial.tint) || !finite(slotMaterial.uvTransform) || !std::isfinite(slotMaterial.emissive) || slotMaterial.emissive < 0)
                { error = "Invalid mesh slot material values"; return {}; }
                slotConstants.tint = m_settings.tint;
                for (size_t component = 0; component < slotConstants.tint.size(); ++component) slotConstants.tint[component] *= slotMaterial.tint[component];
                slotConstants.uvTransform = slotMaterial.uvTransform;
                slotConstants.shading[0] = m_settings.emissive*slotMaterial.emissive;
                for (size_t parameter = 0; parameter < slotConstants.dynamicDefaults.size(); ++parameter)
                {
                    slotConstants.dynamicDefaults[parameter] = slotMaterial.hasDynamicDefaults ? slotMaterial.dynamicDefaults[parameter] : m_settings.dynamicParameters[parameter].defaultValue;
                    if (!finite(slotConstants.dynamicDefaults[parameter])) { error = "Dynamic material parameters must be finite"; return {}; }
                }
                RHIBufferUploadDesc slotUpload; slotUpload.data = &slotConstants; slotUpload.dataSize = sizeof(slotConstants);
                slot.constants = m_device->CreateBuffer(desc,slotUpload);
                if (!slot.constants) { error = "Mesh slot constant allocation failed"; return {}; }
                slot.texture = makeTexture(slotMaterial); slot.sampler = m_device->CreateSampler(slotMaterial.sampler);
                if (!slot.texture || !slot.sampler) { error = "Mesh slot texture or sampler creation failed"; return {}; }
                slot.bindings = makeBindings(slot.constants,slot.texture,slot.sampler);
                if (!slot.bindings) { error = "Mesh slot binding creation failed"; return {}; }
            }
            if (!PrepareMaterial(slot.material,slotMaterial,error)) return {};
            if (!slot.material.pipeline) slot.material.pipeline = m_pipeline;
        }
    }
    error.clear(); return resources;
}

bool ParticleRenderer::PrepareMaterial(ParticleDrawResources::MaterialBinding& binding, const SpriteMaterial& material, std::string& error) const
{
    using namespace Render::RHI;
    if (!material.program)
    {
        if (m_settings.useMaterialShader) { error = "ParticleSprite material shader snapshot is missing"; return false; }
        return true;
    }
    const auto& program = *material.program;
    if (program.abiVersion != kParticleMaterialAbiVersion)
    { error = "Particle material ABI is incompatible; reimport its shader"; return false; }
    auto found = std::find_if(m_materialPipelines.begin(),m_materialPipelines.end(),[&](const auto& pipeline) { return pipeline.program == material.program; });
    if (found == m_materialPipelines.end())
    {
        const auto fragment = std::find_if(program.fragments.begin(),program.fragments.end(),[&](const auto& module)
        { return module.targetBackend == m_device->GetAdapter()->GetBackendType() && module.stage == ShaderStage::Fragment && module.entryPoint == "ParticleMaterialPS"; });
        if (fragment == program.fragments.end() || fragment->bytecode.empty())
        { error = "Particle material has no compiled fragment for this rendering backend"; return false; }
        for (const auto& entry : program.layout.entries)
            if (entry.set != BindingPointMap::kMaterialDescriptorSet || entry.registerSpace != BindingPointMap::kMaterialBindingSpace || entry.count != 1 || entry.binding >= 16 ||
                (entry.type != BindingType::UniformBuffer && entry.type != BindingType::Texture && entry.type != BindingType::Sampler))
            { error = "Particle material resource layout is incompatible"; return false; }
        MaterialPipeline pipeline; pipeline.program = material.program;
        pipeline.layout = m_device->CreateBindingLayout(program.layout);
        auto desc = m_pipeline->GetDesc();
        auto layout = desc.pipelineLayout->GetDesc(); layout.bindingLayouts[BindingPointMap::kMaterialDescriptorSet] = pipeline.layout;
        desc.pipelineLayout = m_device->CreatePipelineLayout(layout); desc.fragmentShader = m_device->CreateShaderModule(*fragment);
        desc.debugName = "ParticleMaterial:"+program.identity;
        if (!pipeline.layout || !desc.pipelineLayout || !desc.fragmentShader || !(pipeline.pipeline = m_device->CreateGraphicsPipeline(desc)))
        { error = "Particle material pipeline creation failed"; return false; }
        if (m_materialPipelines.size() >= 16) m_materialPipelines.erase(m_materialPipelines.begin());
        m_materialPipelines.push_back(std::move(pipeline)); found = std::prev(m_materialPipelines.end());
    }
    RHIBindingSetDesc bindings; bindings.layout = found->layout; bindings.debugName = "ParticleMaterialSnapshot";
    for (const auto& slot : program.layout.entries)
    {
        RHIBindingSetEntry entry; entry.type = slot.type; entry.binding = slot.binding;
        switch (slot.type)
        {
        case BindingType::UniformBuffer:
        {
            const auto value = std::find_if(material.constants.begin(),material.constants.end(),[&](const auto& constant) { return constant.binding == slot.binding; });
            if (value == material.constants.end() || value->bytes.empty() || value->bytes.size() > 65536)
            { error = "Particle material constant snapshot is missing or oversized: "+slot.name; return false; }
            RHIBufferDesc desc; desc.size = value->bytes.size(); desc.usage = BufferUsageFlags::Uniform|BufferUsageFlags::CopySrc; desc.memoryUsage = MemoryUsage::CPUToGPU;
            desc.debugName = slot.name; RHIBufferUploadDesc upload; upload.data = value->bytes.data(); upload.dataSize = value->bytes.size();
            entry.buffer = m_device->CreateBuffer(desc,upload); entry.bufferRange = value->bytes.size();
            if (!entry.buffer) { error = "Particle material constant upload failed"; return false; }
            binding.constants.push_back(entry.buffer); break;
        }
        case BindingType::Texture:
        {
            const auto value = std::find_if(material.textures.begin(),material.textures.end(),[&](const auto& texture) { return texture.binding == slot.binding; });
            if (value == material.textures.end()) { error = "Particle material texture snapshot is missing: "+slot.name; return false; }
            entry.textureView = m_white;
            if (value->texture)
            {
                const auto& desc = value->texture->GetDesc();
                if (desc.dimension != TextureDimension::Texture2D || desc.arrayLayers != 1 || desc.sampleCount != 1 || !HasTextureUsage(desc.usage,TextureUsageFlags::Sampled))
                { error = "Particle material textures must be sampled 2D textures"; return false; }
                RHITextureViewDesc view; view.format = desc.format; view.colorSpace = desc.colorSpace;
                entry.textureView = m_device->CreateTextureView(value->texture,view);
            }
            if (!entry.textureView) { error = "Particle material texture view creation failed"; return false; }
            binding.textures.push_back(entry.textureView); break;
        }
        case BindingType::Sampler:
        {
            const auto value = std::find_if(material.samplers.begin(),material.samplers.end(),[&](const auto& sampler) { return sampler.binding == slot.binding; });
            if (value == material.samplers.end() || !(entry.sampler = m_device->CreateSampler(value->sampler)))
            { error = "Particle material sampler creation failed"; return false; }
            binding.samplers.push_back(entry.sampler); break;
        }
        default: error = "Unsupported particle material binding"; return false;
        }
        bindings.entries.push_back(std::move(entry));
    }
    binding.bindings = m_device->CreateBindingSet(bindings); binding.pipeline = found->pipeline;
    if (!binding.bindings) { error = "Particle material binding creation failed"; return false; }
    return true;
}
}
