#include "ParticleSceneRenderer.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <tuple>
#include <utility>

namespace NLS::Engine::Rendering
{
struct ParticleSceneRenderer::State
{
    struct Pipeline
    {
        std::weak_ptr<const VFX::CompiledSystem> system;
        uint64_t device = 0;
        size_t emitter = 0;
        Render::RHI::RHIRenderTargetLayoutDesc targets;
        std::shared_ptr<VFX::ParticleRenderer> renderer;
    };
    std::vector<Pipeline> pipelines;
};

namespace
{
/**
 * View ordering key for one emitter's transparent draw.
 *
 * Emitters integrate and depth sort independently, so nothing inside a single
 * emitter can order particles that belong to a different one. The pass order
 * decides how those independent streams interleave, which is what this key
 * restores.
 *
 * The plane uses the same camera-forward convention as the per-emitter sort, so
 * a larger key is farther away and therefore drawn first. CPU emitters publish
 * their bounds with the frame. GPU emitters publish the latest completed
 * asynchronous readback, so ordering can trail simulation by a few frames. The
 * component origin remains the fallback before the first successful readback.
 */
float EmitterViewDepth(const VFX::SpriteView& camera, const Maths::Matrix4& localToWorld,
    const std::optional<VFX::ParticleBounds>& bounds)
{
    VFX::Value center{};
    if (bounds.has_value() && !bounds->Empty())
        for (size_t component = 0; component < 3; ++component)
            center[component] = (bounds->minimum[component]+bounds->maximum[component])*0.5f;
    float depth = 0;
    for (size_t row = 0; row < 3; ++row)
    {
        float world = localToWorld.data[row*4+3];
        for (size_t column = 0; column < 3; ++column)
            world += localToWorld.data[row*4+column]*center[column];
        depth += world*camera.cameraForward[row];
    }
    return std::isfinite(depth) ? depth : 0.0f;
}
}
ParticleSceneRenderer::ParticleSceneRenderer() : m_state(std::make_unique<State>()) {}
ParticleSceneRenderer::~ParticleSceneRenderer() = default;

bool ParticleSceneRenderer::AppendPass(std::shared_ptr<Render::RHI::RHIDevice> device, Render::Context::RenderScenePackage& package,
    const Render::FrameGraph::FrozenFrameDescriptor& view, const std::vector<ParticleViewInput>& particles, std::string& error)
{
    namespace rhi = Render::RHI;
    namespace context = Render::Context;
    if (particles.empty()) { error.clear(); return true; }
    if (!device || !view.descriptor.camera) { error = "Particle scene rendering requires a device and frozen camera"; return false; }
    std::erase_if(m_state->pipelines,[](const auto& pipeline) { return pipeline.system.expired(); });
    const auto sameTargets = [](const auto& a, const auto& b)
    { return a.colorFormats == b.colorFormats && a.colorSpaces == b.colorSpaces && a.hasDepth == b.hasDepth && a.depthFormat == b.depthFormat; };
    auto color = package.externalSceneOutputColorView;
    auto depth = package.externalSceneOutputDepthView;
    bool hasSceneDepth = false;
    for (const auto& previous : package.passCommandInputs)
    {
        const bool executes = previous.drawCount != 0 || !previous.recordedDrawCommands.empty();
        hasSceneDepth |= executes && previous.usesDepthStencilAttachment && previous.writesDepthStencilAttachment;
        if (previous.kind == context::RenderPassCommandKind::GBuffer && previous.depthStencilAttachmentView)
            depth = previous.depthStencilAttachmentView;
    }
    for (auto iterator = package.passCommandInputs.rbegin(); iterator != package.passCommandInputs.rend(); ++iterator)
        if (iterator->kind == context::RenderPassCommandKind::Transparent)
        {
            if (!iterator->colorAttachmentViews.empty()) color = iterator->colorAttachmentViews[0];
            if (iterator->depthStencilAttachmentView) depth = iterator->depthStencilAttachmentView;
            break;
        }
    rhi::RHIRenderTargetLayoutDesc targets;
    targets.colorFormats = {color ? color->GetDesc().format : rhi::TextureFormat::RGBA8};
    targets.colorSpaces = {color ? color->GetDesc().colorSpace : rhi::TextureColorSpace::Linear};
    targets.hasDepth = depth != nullptr || (package.targetsSwapchain && hasSceneDepth);
    if (depth) targets.depthFormat = depth->GetDesc().format;
    else if (targets.hasDepth) targets.depthFormat = rhi::TextureFormat::Depth24Stencil8;
    for (const bool solid : {true,false})
    {
        size_t insertionPass = package.passCommandInputs.size();
        if (solid)
            for (size_t i = 0; i < package.passCommandInputs.size(); ++i)
                if (package.passCommandInputs[i].kind == context::RenderPassCommandKind::Transparent)
                { insertionPass = i; break; }
        bool colorBefore = false, depthBefore = false;
        for (size_t i = 0; i < insertionPass; ++i)
        {
            const auto& previous = package.passCommandInputs[i];
            const bool executes = previous.drawCount || !previous.recordedDrawCommands.empty();
            colorBefore |= executes && previous.usesColorAttachment && previous.kind != context::RenderPassCommandKind::GBuffer;
            depthBefore |= executes && previous.usesDepthStencilAttachment && previous.writesDepthStencilAttachment;
        }
        context::RenderPassCommandInput pass;
        pass.kind = solid ? context::RenderPassCommandKind::Opaque : context::RenderPassCommandKind::Transparent;
        pass.debugName = solid ? "VfxSceneSolidSprites" : "VfxSceneSprites";
        pass.renderWidth = package.renderWidth; pass.renderHeight = package.renderHeight; pass.targetsSwapchain = package.targetsSwapchain && !color;
        pass.usesColorAttachment = true; pass.usesDepthStencilAttachment = targets.hasDepth;
        pass.requiresDependencyVisibility = !package.passCommandInputs.empty();
        pass.clearColor = !colorBefore; pass.clearColorValue = package.clearColorValue;
        pass.clearDepth = targets.hasDepth && !depthBefore;
        pass.writesDepthStencilAttachment = targets.hasDepth && (solid || pass.clearDepth);
        if (color) pass.colorAttachmentViews = {color}; pass.depthStencilAttachmentView = depth;
        context::TextureResourceAccess colorAccess;
        colorAccess.texture = color ? color->GetTexture() : nullptr;
        if (color) colorAccess.subresourceRange = color->GetDesc().subresourceRange;
        colorAccess.mode = context::ResourceAccessMode::Write; colorAccess.state = rhi::ResourceState::RenderTarget;
        colorAccess.stages = rhi::PipelineStageMask::RenderTarget;
        colorAccess.access = rhi::AccessMask::ColorAttachmentRead|rhi::AccessMask::ColorAttachmentWrite;
        pass.textureResourceAccesses.push_back(colorAccess);
        if (depth)
        {
            context::TextureResourceAccess access;
            access.texture = depth->GetTexture(); access.subresourceRange = depth->GetDesc().subresourceRange;
            access.mode = pass.writesDepthStencilAttachment ? context::ResourceAccessMode::Write : context::ResourceAccessMode::Read;
            access.state = pass.writesDepthStencilAttachment ? rhi::ResourceState::DepthWrite : rhi::ResourceState::DepthRead;
            access.stages = rhi::PipelineStageMask::DepthStencil;
            access.access = pass.writesDepthStencilAttachment ? rhi::AccessMask::DepthStencilRead|rhi::AccessMask::DepthStencilWrite : rhi::AccessMask::DepthStencilRead;
            pass.textureResourceAccesses.push_back(access);
        }
        const auto viewProjection = view.descriptor.camera->GetProjectionMatrix()*view.descriptor.camera->GetViewMatrix();
        const auto& cameraView = view.descriptor.camera->GetViewMatrix();
        struct DrawOwner
        {
            std::shared_ptr<const VFX::SystemRenderFrame> frame;
            std::shared_ptr<const VFX::SpriteDrawResources> draw;
        };
        std::vector<float> commandDepths;
        for (const auto& input : particles)
        {
            if (!input.proxy || !input.frame) continue;
            if (!input.materialError.empty()) { error = input.materialError; return false; }
            const auto frame = input.proxy->Prepare(device,input.frame,error);
            if (!frame) return false;
            const auto& program = input.frame->system;
            const auto* shaders = program->FindPlatform(device->GetAdapter()->GetBackendType());
            for (size_t index = 0; index < frame->emitters.size(); ++index)
            {
                const auto& data = frame->emitters[index]; if (!data) continue;
                const auto& settings = program->asset.emitters[index].renderer == VFX::RendererType::Ribbon
                    ? VFX::ResolveRibbonDrawSettings(program->asset.emitters[index].ribbon)
                    : program->asset.emitters[index].sprite;
                if (solid != (settings.blend == VFX::SpriteBlendMode::Masked || settings.blend == VFX::SpriteBlendMode::Opaque)) continue;
                auto found = std::find_if(m_state->pipelines.begin(),m_state->pipelines.end(),[&](const auto& pipeline)
                { return pipeline.system.lock() == program && pipeline.device == device->GetCacheIdentity() && pipeline.emitter == index && sameTargets(pipeline.targets,targets); });
                if (found == m_state->pipelines.end())
                {
                    auto renderer = std::make_shared<VFX::ParticleRenderer>();
                    if (!renderer->Initialize(device,program->asset.emitters[index].attributes,shaders->emitters[index].spriteVertex,
                        shaders->emitters[index].spriteFragment,targets,error,
                        program->asset.emitters[index].sortByViewDepth ? &shaders->emitters[index].sort : nullptr,
                        &shaders->emitters[index].visibility,&shaders->emitters[index].simulation.bounds,settings,
                        program->asset.emitters[index].renderer == VFX::RendererType::Mesh ? &program->asset.emitters[index].mesh : nullptr)) return false;
                    if (m_state->pipelines.size() >= 256) m_state->pipelines.erase(m_state->pipelines.begin());
                    m_state->pipelines.push_back({program,device->GetCacheIdentity(),index,targets,std::move(renderer)});
                    found = std::prev(m_state->pipelines.end());
                }
                VFX::SpriteView camera;
                std::copy_n(viewProjection.data,16,camera.viewProjection.begin());
                std::copy_n(input.localToWorld.data,16,camera.localToWorld.begin());
                camera.cameraRight = {cameraView.data[0],cameraView.data[1],cameraView.data[2],0};
                camera.cameraUp = {cameraView.data[4],cameraView.data[5],cameraView.data[6],0};
                camera.cameraForward = {-cameraView.data[8],-cameraView.data[9],-cameraView.data[10],0};
                camera.frustumCulling = program->asset.emitters[index].frustumCulling;
                const auto& orderingBounds = data->orderingBounds ? data->orderingBounds : data->cpuBounds;
                const float emitterDepth = EmitterViewDepth(camera,input.localToWorld,orderingBounds);
                VFX::SpriteDrawContext drawContext;
                if (index < input.materials.size() && !input.materials[index].empty()) drawContext.material = input.materials[index][0];
                if (index < input.materials.size()) drawContext.meshMaterials = input.materials[index];
                if (index < input.meshes.size()) drawContext.meshes = input.meshes[index];
                const auto& projection = view.descriptor.camera->GetProjectionMatrix();
                drawContext.depthProjection = {projection.data[10],projection.data[11],projection.data[14],projection.data[15]};
                if (settings.softDistance > 0 && depth && !pass.writesDepthStencilAttachment && depth->GetTexture()->GetDesc().sampleCount == 1 &&
                    rhi::HasTextureUsage(depth->GetTexture()->GetDesc().usage,rhi::TextureUsageFlags::Sampled))
                {
                    drawContext.sceneDepth = depth;
                    drawContext.sceneDepthState = rhi::ResourceState::DepthRead|rhi::ResourceState::ShaderRead;
                    auto& access = pass.textureResourceAccesses[1]; access.state = drawContext.sceneDepthState;
                    access.stages = access.stages|rhi::PipelineStageMask::FragmentShader;
                    access.access = access.access|rhi::AccessMask::ShaderRead;
                }
                auto draw = found->renderer->Prepare(*data,camera,error,drawContext); if (!draw) return false;
                for (const auto& texture : {draw->texture,draw->usesSceneDepth ? nullptr : draw->sceneDepth})
                    if (texture) pass.textureResourceAccesses.push_back({texture->GetTexture(),texture->GetDesc().subresourceRange,context::ResourceAccessMode::Read,
                        rhi::ResourceState::ShaderRead,rhi::PipelineStageMask::FragmentShader,rhi::AccessMask::ShaderRead});
                pass.computeDispatchInputs.insert(pass.computeDispatchInputs.end(),draw->dispatches.begin(),draw->dispatches.end());
                if (draw->sorting || draw->visibility)
                {
                    const auto idState = data->ids->GetDesc().memoryUsage == rhi::MemoryUsage::CPUToGPU ? rhi::ResourceState::GenericRead : rhi::ResourceState::ShaderRead;
                    pass.bufferResourceAccesses.push_back({data->ids,context::ResourceAccessMode::Read,idState,rhi::PipelineStageMask::ComputeShader,rhi::AccessMask::ShaderRead});
                    const auto keys = draw->sorting ? draw->sorting->keys : draw->visibility->keys;
                    pass.bufferResourceAccesses.push_back({keys,context::ResourceAccessMode::Write,rhi::ResourceState::ShaderRead,
                        rhi::PipelineStageMask::ComputeShader|rhi::PipelineStageMask::VertexShader,rhi::AccessMask::ShaderWrite|rhi::AccessMask::ShaderRead});
                }
                context::RecordedDrawCommandInput command;
                command.pipeline = draw->pipeline; command.passBindingSet = draw->bindings;
                command.materialBindingSet = draw->materialBindings;
                for (const auto& slot : draw->meshSlots)
                    for (const auto& buffer : {slot.mesh->GetVertexBuffer(),slot.mesh->GetIndexBuffer()})
                        pass.bufferResourceAccesses.push_back({buffer,context::ResourceAccessMode::Read,
                            buffer->GetDesc().memoryUsage == rhi::MemoryUsage::CPUToGPU ? rhi::ResourceState::GenericRead : rhi::ResourceState::VertexBuffer|rhi::ResourceState::IndexBuffer,
                            rhi::PipelineStageMask::VertexInput,rhi::AccessMask::VertexRead|rhi::AccessMask::IndexRead});
                for (const auto& slot : draw->meshSlots)
                {
                    pass.textureResourceAccesses.push_back({slot.texture->GetTexture(),slot.texture->GetDesc().subresourceRange,context::ResourceAccessMode::Read,
                        rhi::ResourceState::ShaderRead,rhi::PipelineStageMask::FragmentShader,rhi::AccessMask::ShaderRead});
                    for (const auto& texture : slot.material.textures)
                        pass.textureResourceAccesses.push_back({texture->GetTexture(),texture->GetDesc().subresourceRange,context::ResourceAccessMode::Read,
                            rhi::ResourceState::ShaderRead,rhi::PipelineStageMask::FragmentShader,rhi::AccessMask::ShaderRead});
                }
                for (const auto& texture : draw->materialTextures)
                    pass.textureResourceAccesses.push_back({texture->GetTexture(),texture->GetDesc().subresourceRange,context::ResourceAccessMode::Read,
                        rhi::ResourceState::ShaderRead,rhi::PipelineStageMask::FragmentShader,rhi::AccessMask::ShaderRead});
                command.indirectArguments = draw->arguments;
                command.resourceOwner = std::make_shared<const DrawOwner>(DrawOwner{frame,draw}); command.instanceCount = 1;
                if (draw->meshSlots.empty())
                {
                    command.indirectArgumentOffset = 4*sizeof(uint32_t);
                    pass.recordedDrawCommands.push_back(std::move(command));
                    commandDepths.push_back(emitterDepth);
                }
                else for (const auto& slot : draw->meshSlots)
                {
                    auto slotCommand = command;
                    slotCommand.mesh = slot.mesh;
                    slotCommand.pipeline = slot.material.pipeline ? slot.material.pipeline : draw->pipeline;
                    slotCommand.passBindingSet = slot.bindings ? slot.bindings : draw->bindings;
                    slotCommand.materialBindingSet = slot.material.bindings ? slot.material.bindings : draw->materialBindings;
                    slotCommand.indirectArgumentOffset = slot.argumentOffset;
                    pass.recordedDrawCommands.push_back(std::move(slotCommand));
                    commandDepths.push_back(emitterDepth);
                }
                const auto readState = data->values->GetDesc().memoryUsage == rhi::MemoryUsage::CPUToGPU ? rhi::ResourceState::GenericRead : rhi::ResourceState::ShaderRead;
                const auto argumentState = draw->arguments->GetDesc().memoryUsage == rhi::MemoryUsage::CPUToGPU ? rhi::ResourceState::GenericRead : rhi::ResourceState::IndirectArgument;
                const auto sortStage = draw->dispatches.empty() ? rhi::PipelineStageMask::None : rhi::PipelineStageMask::ComputeShader;
                const auto sortAccess = draw->dispatches.empty() ? rhi::AccessMask::None : rhi::AccessMask::ShaderRead;
                pass.bufferResourceAccesses.push_back({data->values,context::ResourceAccessMode::Read,readState,rhi::PipelineStageMask::VertexShader|sortStage,rhi::AccessMask::ShaderRead});
                pass.bufferResourceAccesses.push_back({draw->arguments,context::ResourceAccessMode::Write,argumentState,rhi::PipelineStageMask::DrawIndirect|sortStage,rhi::AccessMask::IndirectRead|rhi::AccessMask::ShaderWrite});
                if (data->bounds) pass.bufferResourceAccesses.push_back({data->bounds,context::ResourceAccessMode::Read,
                    data->bounds->GetDesc().memoryUsage == rhi::MemoryUsage::CPUToGPU ? rhi::ResourceState::GenericRead : rhi::ResourceState::ShaderRead,
                    rhi::PipelineStageMask::ComputeShader,rhi::AccessMask::ShaderRead});
                pass.bufferResourceAccesses.push_back({data->counters,context::ResourceAccessMode::Read,
                    data->counters->GetDesc().memoryUsage == rhi::MemoryUsage::CPUToGPU ? rhi::ResourceState::GenericRead : rhi::ResourceState::IndirectArgument,
                    rhi::PipelineStageMask::ComputeShader,rhi::AccessMask::ShaderRead});
                pass.bufferVisibilityTransitions.push_back({data->values,rhi::ResourceState::Unknown,readState,rhi::PipelineStageMask::AllCommands,rhi::PipelineStageMask::VertexShader,
                    rhi::AccessMask::MemoryRead|rhi::AccessMask::MemoryWrite,rhi::AccessMask::ShaderRead});
                pass.bufferVisibilityTransitions.push_back({draw->arguments,rhi::ResourceState::Unknown,argumentState,rhi::PipelineStageMask::AllCommands,rhi::PipelineStageMask::DrawIndirect,
                    rhi::AccessMask::MemoryRead|rhi::AccessMask::MemoryWrite,rhi::AccessMask::IndirectRead});
            }
        }
        if (!solid && pass.recordedDrawCommands.size() > 1)
        {
            // Emitters are simulated and sorted independently, so their
            // transparent streams would otherwise interleave in declaration
            // order. A stable far-to-near sort keeps the declared order for
            // equal depths (for example same-component GPU emitters, whose
            // bounds stay on the device) while ordering everything the CPU can
            // resolve. Opaque particles write depth and need no ordering.
            std::vector<std::pair<float,std::size_t>> order;
            order.reserve(pass.recordedDrawCommands.size());
            for (std::size_t command = 0; command < pass.recordedDrawCommands.size(); ++command)
                order.push_back({command < commandDepths.size() ? commandDepths[command] : 0.0f,command});
            std::stable_sort(order.begin(),order.end(),[](const auto& left,const auto& right)
            { return left.first > right.first; });
            std::vector<context::RecordedDrawCommandInput> sorted;
            sorted.reserve(pass.recordedDrawCommands.size());
            for (const auto& entry : order) sorted.push_back(std::move(pass.recordedDrawCommands[entry.second]));
            pass.recordedDrawCommands = std::move(sorted);
        }
        if (pass.recordedDrawCommands.empty()) continue;
        pass.drawCount = pass.recordedDrawCommands.size();
        package.visibleDrawCount += pass.drawCount; package.drawCommandCount += pass.drawCount;
        if (solid) { package.opaqueDrawCount += pass.drawCount; package.hasOpaquePass = true; }
        else { package.transparentDrawCount += pass.drawCount; package.hasTransparentPass = true; }
        package.hasVisibleDraws = true; package.containsCommandInputs = true;
        // Build through the shared slicer so dependency metadata and draw ownership
        // remain identical to ordinary scene passes, including particle-only scenes.
        if (package.parallelCommandWorkUnits.empty())
        {
            for (size_t index = 0; index < package.passCommandInputs.size(); ++index)
            {
                auto units = context::BuildRecordedDrawCommandWorkUnitsForPass(package.passCommandInputs[index],index,package.parallelCommandWorkUnits.size());
                package.parallelCommandWorkUnits.insert(package.parallelCommandWorkUnits.end(),std::make_move_iterator(units.begin()),std::make_move_iterator(units.end()));
            }
        }
        const size_t passIndex = insertionPass;
        // A particle-only opaque pass can become the first initializer of a target.
        // Later mesh passes must then load it instead of clearing away the particles.
        const auto preserveInitializedAttachments = [&](auto& successor)
        {
            if (successor.colorAttachmentViews == pass.colorAttachmentViews && successor.targetsSwapchain == pass.targetsSwapchain) successor.clearColor = false;
            if (successor.depthStencilAttachmentView == pass.depthStencilAttachmentView && successor.targetsSwapchain == pass.targetsSwapchain) successor.clearDepth = false;
        };
        for (size_t i = passIndex; i < package.passCommandInputs.size(); ++i) preserveInitializedAttachments(package.passCommandInputs[i]);
        size_t unitIndex = package.parallelCommandWorkUnits.size();
        for (size_t i = 0; i < package.parallelCommandWorkUnits.size(); ++i)
            if (package.parallelCommandWorkUnits[i].sourcePassIndex >= passIndex &&
                package.parallelCommandWorkUnits[i].sourcePassIndex != context::kInvalidParallelCommandSourcePassIndex)
            { unitIndex = i; break; }
        auto units = context::BuildRecordedDrawCommandWorkUnitsForPass(pass,passIndex,unitIndex);
        const auto remap = [&](uint64_t& index) { if (index >= unitIndex) index += units.size(); };
        const auto remapEdge = [&](auto& edge) { remap(edge.sourceWorkUnitIndex); remap(edge.targetWorkUnitIndex); };
        for (auto& previous : package.parallelCommandWorkUnits)
        {
            remap(previous.workUnitIndex); remap(previous.submissionOrder);
            if (previous.sourcePassIndex >= passIndex && previous.sourcePassIndex != context::kInvalidParallelCommandSourcePassIndex) ++previous.sourcePassIndex;
            if (previous.sourcePassIndex > passIndex && previous.sourcePassIndex != context::kInvalidParallelCommandSourcePassIndex) preserveInitializedAttachments(previous.commandInput);
            if (previous.commandInput.dependencySourceWorkUnitIndex) remap(*previous.commandInput.dependencySourceWorkUnitIndex);
            for (auto& edge : previous.incomingDependencyEdges) remapEdge(edge);
        }
        for (auto& previous : package.passCommandInputs)
            if (previous.dependencySourceWorkUnitIndex) remap(*previous.dependencySourceWorkUnitIndex);
        for (auto& edge : package.workUnitDependencyEdges) remapEdge(edge);
        if (unitIndex > 0)
        {
            context::WorkUnitDependencyEdge edge;
            edge.sourceWorkUnitIndex = package.parallelCommandWorkUnits[unitIndex-1].workUnitIndex;
            edge.targetWorkUnitIndex = units.front().workUnitIndex;
            const auto& previous = package.parallelCommandWorkUnits[unitIndex-1].commandInput;
            if (previous.drawCount || !previous.recordedDrawCommands.empty())
            {
                edge.resourceKind = context::ThreadedDependencyResourceKind::Texture;
                edge.sourceTextureAccess = colorAccess; edge.targetTextureAccess = colorAccess;
            }
            units.front().incomingDependencyEdges.push_back(edge); package.workUnitDependencyEdges.push_back(std::move(edge));
        }
        if (depth && !pass.clearDepth)
            for (auto previous = std::make_reverse_iterator(package.parallelCommandWorkUnits.begin()+unitIndex); previous != package.parallelCommandWorkUnits.rend(); ++previous)
            {
                const auto& input = previous->commandInput;
                if (input.depthStencilAttachmentView != depth || !input.writesDepthStencilAttachment) continue;
                context::WorkUnitDependencyEdge edge;
                edge.sourceWorkUnitIndex = previous->workUnitIndex; edge.targetWorkUnitIndex = units.front().workUnitIndex;
                edge.resourceKind = context::ThreadedDependencyResourceKind::Texture;
                auto source = pass.textureResourceAccesses[1]; source.mode = context::ResourceAccessMode::Write;
                source.state = rhi::ResourceState::DepthWrite; source.access = rhi::AccessMask::DepthStencilWrite;
                edge.sourceTextureAccess = source; edge.targetTextureAccess = pass.textureResourceAccesses[1];
                units.front().incomingDependencyEdges.push_back(edge); package.workUnitDependencyEdges.push_back(std::move(edge));
                break;
            }
        if (unitIndex < package.parallelCommandWorkUnits.size())
        {
            auto& successor = package.parallelCommandWorkUnits[unitIndex];
            context::WorkUnitDependencyEdge edge;
            edge.sourceWorkUnitIndex = units.back().workUnitIndex; edge.targetWorkUnitIndex = successor.workUnitIndex;
            edge.resourceKind = context::ThreadedDependencyResourceKind::Texture;
            edge.sourceTextureAccess = colorAccess; edge.targetTextureAccess = colorAccess;
            successor.incomingDependencyEdges.push_back(edge); package.workUnitDependencyEdges.push_back(edge);
            if (depth && pass.writesDepthStencilAttachment)
            {
                edge.sourceTextureAccess = pass.textureResourceAccesses[1];
                auto read = *edge.sourceTextureAccess; read.mode = context::ResourceAccessMode::Read;
                read.state = rhi::ResourceState::DepthRead; read.access = rhi::AccessMask::DepthStencilRead;
                edge.targetTextureAccess = read;
                successor.incomingDependencyEdges.push_back(edge); package.workUnitDependencyEdges.push_back(edge);
            }
        }
        package.parallelCommandWorkUnits.insert(package.parallelCommandWorkUnits.begin()+unitIndex,std::make_move_iterator(units.begin()),std::make_move_iterator(units.end()));
        package.containsParallelCommandWorkUnits = true; package.parallelCommandWorkUnitCount = package.parallelCommandWorkUnits.size();
        package.parallelDrawCommandBatches = context::BuildParallelDrawCommandBatchMetadata(package.parallelCommandWorkUnits);
        package.materialBatchCount += pass.drawCount;
        package.passCommandInputs.insert(package.passCommandInputs.begin()+passIndex,std::move(pass)); package.passPlanCount = package.passCommandInputs.size();
    }
    error.clear(); return true;
}
}
