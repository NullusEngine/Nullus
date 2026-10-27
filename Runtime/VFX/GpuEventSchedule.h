#pragma once

#include "VfxGraph.h"
#include <algorithm>
#include <string>
#include <vector>

namespace NLS::VFX
{
// A substep runs publishers before their receivers, independently of asset order.
// Cycles have no well-defined same-step event stream and are diagnosed at compile time.
inline bool BuildGpuEventOrder(const SystemAsset& asset,std::vector<size_t>& order,std::string& error)
{
    order.clear();
    if (asset.emitters.size() > 256 && std::any_of(asset.emitters.begin(),asset.emitters.end(),
        [](const auto& emitter) { return emitter.target == SimulationTarget::GPU; }))
    { error = "GPU event storage supports at most 256 emitters per system"; return false; }
    std::vector<std::vector<size_t>> edges(asset.emitters.size());
    std::vector<size_t> degree(asset.emitters.size());
    size_t gpuCount = 0;
    for (size_t receiver = 0; receiver < asset.emitters.size(); ++receiver)
    {
        if (asset.emitters[receiver].target != SimulationTarget::GPU) continue;
        ++gpuCount;
        for (const auto& handler : asset.emitters[receiver].eventHandlers)
        {
            if (handler.receiver.Empty()) continue;
            if (handler.receiver.sourceEmitter == asset.emitters[receiver].id)
            { error = "GPU event handler cannot receive its own emitter"; return false; }
            bool foundSource = handler.receiver.sourceEmitter.empty();
            for (size_t source = 0; source < asset.emitters.size(); ++source)
            {
                if (!handler.receiver.sourceEmitter.empty() && asset.emitters[source].id != handler.receiver.sourceEmitter) continue;
                foundSource = true;
                if (source == receiver || asset.emitters[source].target != SimulationTarget::GPU ||
                    !(asset.emitters[source].eventGenerationMask & EventBit(handler.receiver.eventKind))) continue;
                if (std::find(edges[source].begin(),edges[source].end(),receiver) == edges[source].end())
                { edges[source].push_back(receiver); ++degree[receiver]; }
            }
            if (!foundSource) { error = "GPU event handler source emitter is missing"; return false; }
        }
    }
    for (size_t source = 0; source < degree.size(); ++source)
        if (asset.emitters[source].target == SimulationTarget::GPU && degree[source] == 0) order.push_back(source);
    for (size_t cursor = 0; cursor < order.size(); ++cursor)
        for (const auto receiver : edges[order[cursor]]) if (--degree[receiver] == 0) order.push_back(receiver);
    if (order.size() != gpuCount) { error = "GPU event dependency cycle has no same-step execution order"; order.clear(); return false; }
    error.clear(); return true;
}
}
