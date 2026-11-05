#include "Rendering/Context/TransparentSubmission.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace NLS::Render::Context
{
namespace
{
bool Compatible(const RenderPassCommandInput& a, const RenderPassCommandInput& b)
{
    if (b.readbackTextureGeneration != 0 || a.queueDependencyPolicy != b.queueDependencyPolicy) return false;
    // Preparation which reads a preceding pass's attachments must stay after
    // that pass's draws; it cannot move to the front of a combined pass.
    for (const auto& dispatch : b.computeDispatchInputs)
        for (const auto& access : dispatch.textureResourceAccesses)
        {
            if (!access.texture) continue;
            for (const auto& color : a.colorAttachmentViews)
                if (color && color->GetTexture() == access.texture) return false;
            if (a.writesDepthStencilAttachment && a.depthStencilAttachmentView && a.depthStencilAttachmentView->GetTexture() == access.texture) return false;
        }
    return a.kind == RenderPassCommandKind::Transparent && b.kind == a.kind &&
        a.colorAttachmentViews == b.colorAttachmentViews && a.depthStencilAttachmentView == b.depthStencilAttachmentView &&
        a.targetsSwapchain == b.targetsSwapchain && a.renderWidth == b.renderWidth && a.renderHeight == b.renderHeight &&
        a.outputViewport == b.outputViewport && !b.clearColor && !b.clearDepth && !b.clearStencil &&
        a.queueType == b.queueType;
}
template<class T> void Append(std::vector<T>& to, const std::vector<T>& from)
{ to.insert(to.end(), from.begin(), from.end()); }
void Merge(RenderPassCommandInput& to, const RenderPassCommandInput& from)
{
    Append(to.recordedDrawCommands, from.recordedDrawCommands);
    Append(to.computeDispatchInputs, from.computeDispatchInputs);
    Append(to.bufferResourceAccesses, from.bufferResourceAccesses);
    Append(to.textureResourceAccesses, from.textureResourceAccesses);
    Append(to.bufferVisibilityTransitions, from.bufferVisibilityTransitions);
    Append(to.textureVisibilityTransitions, from.textureVisibilityTransitions);
    Append(to.exportedBufferVisibilityTransitions, from.exportedBufferVisibilityTransitions);
    Append(to.exportedTextureVisibilityTransitions, from.exportedTextureVisibilityTransitions);
    to.requiresFrameData |= from.requiresFrameData; to.requiresObjectData |= from.requiresObjectData;
    to.requiresLightingData |= from.requiresLightingData;
    to.requiresDependencyVisibility |= from.requiresDependencyVisibility;
    to.usesDepthStencilAttachment |= from.usesDepthStencilAttachment;
    to.writesDepthStencilAttachment |= from.writesDepthStencilAttachment;
    to.drawCount += from.drawCount;
}
}

void FinalizeTransparentSubmission(RenderScenePackage& package)
{
    for (const auto& unit : package.parallelCommandWorkUnits)
        if (unit.sourcePassIndex != kInvalidParallelCommandSourcePassIndex && unit.sourcePassIndex >= package.passCommandInputs.size()) return;
    if (std::none_of(package.passCommandInputs.begin(), package.passCommandInputs.end(), [](const auto& pass)
        { return pass.kind == RenderPassCommandKind::Transparent && std::any_of(pass.recordedDrawCommands.begin(), pass.recordedDrawCommands.end(),
            [](const auto& draw) { return draw.transparentSortKey.has_value(); }); })) return;
    if (package.parallelCommandWorkUnits.empty())
        for (size_t p = 0; p < package.passCommandInputs.size(); ++p)
        {
            auto units = BuildRecordedDrawCommandWorkUnitsForPass(package.passCommandInputs[p], p, package.parallelCommandWorkUnits.size());
            Append(package.parallelCommandWorkUnits, units);
        }

    std::vector<RenderPassCommandInput> passes;
    std::vector<size_t> passMap(package.passCommandInputs.size());
    for (size_t p = 0; p < package.passCommandInputs.size();)
    {
        const size_t first = p;
        auto pass = package.passCommandInputs[p++];
        if (pass.kind == RenderPassCommandKind::Transparent)
        {
            while (p < package.passCommandInputs.size() && Compatible(pass, package.passCommandInputs[p])) Merge(pass, package.passCommandInputs[p++]);
            std::stable_sort(pass.recordedDrawCommands.begin(), pass.recordedDrawCommands.end(), [](const auto& a, const auto& b)
            {
                // Unkeyed editor overlays retain their final position.
                if (!a.transparentSortKey || !b.transparentSortKey) return a.transparentSortKey.has_value() && !b.transparentSortKey;
                return Sprites::SpriteSortLess(*a.transparentSortKey, *b.transparentSortKey);
            });
            pass.drawCount = pass.recordedDrawCommands.size();
            pass.debugName = "SceneTransparentSubmission";
        }
        for (size_t original = first; original < p; ++original) passMap[original] = passes.size();
        passes.push_back(std::move(pass));
    }

    // Retain auxiliary units and remap every old dependency to the boundary of
    // the new pass. A merged pass may have a different draw-slice count.
    auto oldUnits = std::move(package.parallelCommandWorkUnits);
    auto edges = std::move(package.workUnitDependencyEdges);
    std::map<uint64_t, std::pair<uint64_t,uint64_t>> unitMap;
    std::map<size_t, std::pair<uint64_t,uint64_t>> passUnits;
    std::vector<ParallelCommandWorkUnit> units;
    for (const auto& old : oldUnits)
    {
        Append(edges, old.incomingDependencyEdges);
        const bool auxiliary = old.sourcePassIndex == kInvalidParallelCommandSourcePassIndex;
        const size_t passIndex = auxiliary ? size_t(-1) : passMap[old.sourcePassIndex];
        if (auxiliary)
        {
            auto copy = old; copy.workUnitIndex = units.size(); copy.submissionOrder = copy.workUnitIndex;
            copy.incomingDependencyEdges.clear(); unitMap[old.workUnitIndex] = {copy.workUnitIndex, copy.workUnitIndex};
            units.push_back(std::move(copy));
        }
        else
        {
            auto found = passUnits.find(passIndex);
            if (found == passUnits.end())
            {
                const auto first = units.size();
                auto built = BuildRecordedDrawCommandWorkUnitsForPass(passes[passIndex], passIndex, first);
                Append(units, built);
                found = passUnits.emplace(passIndex, std::pair<uint64_t,uint64_t>{first,units.size()-1}).first;
            }
            unitMap[old.workUnitIndex] = found->second;
        }
    }
    package.workUnitDependencyEdges.clear();
    const auto textureKey = [](const std::optional<TextureResourceAccess>& access)
    {
        const auto value=access.value_or(TextureResourceAccess{});const auto& range=value.subresourceRange;
        return std::tuple{bool(access),uintptr_t(value.texture.get()),range.baseMipLevel,range.mipLevelCount,
            range.baseArrayLayer,range.arrayLayerCount,int(value.mode),uint64_t(value.state),uint64_t(value.stages),uint64_t(value.access)};
    };
    const auto bufferKey = [](const std::optional<BufferResourceAccess>& access)
    {
        const auto value=access.value_or(BufferResourceAccess{});
        return std::tuple{bool(access),uintptr_t(value.buffer.get()),int(value.mode),uint64_t(value.state),uint64_t(value.stages),uint64_t(value.access)};
    };
    using EdgeKey=decltype(std::tuple{uint64_t{},uint64_t{},int{},int{},textureKey({}),textureKey({}),bufferKey({}),bufferKey({})});
    std::set<EdgeKey> seen;
    const auto connect = [&](WorkUnitDependencyEdge edge)
    {
        if (edge.sourceWorkUnitIndex == edge.targetWorkUnitIndex) return;
        if (!seen.emplace(edge.sourceWorkUnitIndex,edge.targetWorkUnitIndex,int(edge.kind),int(edge.resourceKind),
            textureKey(edge.sourceTextureAccess),textureKey(edge.targetTextureAccess),bufferKey(edge.sourceBufferAccess),bufferKey(edge.targetBufferAccess)).second) return;
        units[edge.targetWorkUnitIndex].incomingDependencyEdges.push_back(edge);
        package.workUnitDependencyEdges.push_back(std::move(edge));
    };
    for (auto edge : edges)
    {
        const auto source = unitMap.find(edge.sourceWorkUnitIndex), target = unitMap.find(edge.targetWorkUnitIndex);
        if (source == unitMap.end() || target == unitMap.end() || source->second == target->second) continue;
        edge.sourceWorkUnitIndex = source->second.second; edge.targetWorkUnitIndex = target->second.first; connect(std::move(edge));
    }
    for (const auto& [pass, range] : passUnits)
        for (auto i = range.first; i < range.second; ++i)
        { WorkUnitDependencyEdge edge; edge.sourceWorkUnitIndex=i; edge.targetWorkUnitIndex=i+1; connect(std::move(edge)); }
    const auto remapDependency = [&](auto& input)
    {
        if (!input.dependencySourceWorkUnitIndex) return;
        const auto found = unitMap.find(*input.dependencySourceWorkUnitIndex);
        if (found != unitMap.end()) input.dependencySourceWorkUnitIndex = found->second.second;
    };
    for (auto& pass : passes) remapDependency(pass);
    for (auto& unit : units) remapDependency(unit.commandInput);
    package.passCommandInputs = std::move(passes); package.parallelCommandWorkUnits = std::move(units);
    package.passPlanCount = package.passCommandInputs.size();
    package.parallelCommandWorkUnitCount = package.parallelCommandWorkUnits.size(); package.containsParallelCommandWorkUnits = !package.parallelCommandWorkUnits.empty();
    package.parallelDrawCommandBatches = BuildParallelDrawCommandBatchMetadata(package.parallelCommandWorkUnits);
}
}
