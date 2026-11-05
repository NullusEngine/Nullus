#include "ECSCooker.h"
#include <ECS/Serialization/WorldArchive.h>
#include <map>
#include <queue>
#include <set>

namespace NLS::Cook::Detail
{
bool OrderECSInputs(std::vector<std::filesystem::path>& paths, CookSummary& summary, const CookOptions& options)
{
    using namespace Core::Assets;
    struct Input { std::filesystem::path path; std::string guid; std::vector<std::string> resources; };
    std::vector<Input> inputs;
    std::vector<std::filesystem::path> other;
    std::map<std::string, size_t> byGuid;
    try
    {
        for (const auto& path : paths)
        {
            if (options.cancel && options.cancel->load()) throw std::runtime_error("ECS cook cancelled");
            const auto meta = AssetMeta::Load(GetAssetMetaPath(path));
            const auto type = meta && meta->assetType != AssetType::Unknown ? meta->assetType : InferAssetType(path);
            if ((type != AssetType::ECSScene && type != AssetType::ECSPrefab) || !meta || !meta->id.IsValid())
            { other.push_back(path); continue; }
            Input input{path, meta->id.ToString(), {}};
            const auto document = ECS::WorldArchive::ReadFile(path);
            for (const auto& resource : document.resources)
            {
                const auto guid = Guid::TryParse(resource.guid);
                if (guid) input.resources.push_back(guid->ToString());
            }
            if (!byGuid.emplace(input.guid, inputs.size()).second) throw std::invalid_argument("Duplicate ECS asset GUID in cook inputs: " + input.guid);
            inputs.push_back(std::move(input));
        }
        std::vector<std::vector<size_t>> dependents(inputs.size());
        std::vector<size_t> degree(inputs.size(), 0);
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            std::set<size_t> dependencies;
            for (const auto& guid : inputs[i].resources)
                if (const auto found = byGuid.find(guid); found != byGuid.end()) dependencies.insert(found->second);
            for (const auto dependency : dependencies) { ++degree[i]; dependents[dependency].push_back(i); }
        }
        std::priority_queue<size_t, std::vector<size_t>, std::greater<size_t>> ready;
        for (size_t i = 0; i < degree.size(); ++i) if (!degree[i]) ready.push(i);
        size_t emitted = 0;
        while (!ready.empty())
        {
            const auto i = ready.top(); ready.pop(); other.push_back(inputs[i].path); ++emitted;
            for (const auto dependent : dependents[i]) if (--degree[dependent] == 0) ready.push(dependent);
        }
        if (emitted != inputs.size())
        {
            std::string message = "ECS artifact resource dependency cycle:";
            for (size_t i = 0; i < inputs.size(); ++i) if (degree[i]) message += " " + inputs[i].path.generic_string();
            throw std::invalid_argument(message);
        }
        paths = std::move(other); return true;
    }
    catch (const std::exception& error)
    {
        summary.failed += 1;
        summary.AddDiagnostic(CookSeverity::Error, "cook-ecs-dependencies", error.what());
        return false;
    }
}
}
