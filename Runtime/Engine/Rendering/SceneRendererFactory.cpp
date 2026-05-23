#include "Rendering/SceneRendererFactory.h"
#include "Rendering/SceneRendererMaterialBinding.h"

#include <algorithm>
#include <utility>

#include "Components/MeshRenderer.h"

namespace NLS::Engine::Rendering
{
std::vector<SceneRendererMaterialArtifactBinding> ResolveSceneRendererMaterialBindings(
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    const NLS::Engine::Assets::RuntimeAssetDatabase& runtimeAssets)
{
    const auto meshRendererTypeName =
        NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName();

    std::vector<SceneRendererMaterialArtifactBinding> bindings;
    for (const auto& object : prefab.graph.objects)
    {
        if (object.typeName != meshRendererTypeName)
            continue;

        const auto materials = std::find_if(
            object.properties.begin(),
            object.properties.end(),
            [](const NLS::Engine::Serialize::PropertyRecord& property)
            {
                return property.name == "materials";
            });
        if (materials == object.properties.end() ||
            materials->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Array)
        {
            continue;
        }

        uint32_t slotIndex = 0u;
        for (const auto& value : materials->value.GetArray())
        {
            SceneRendererMaterialArtifactBinding binding;
            binding.rendererDebugName = object.debugName;
            binding.slotIndex = slotIndex++;

            if (value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference)
            {
                const auto& asset = value.GetObjectReference();
                if (!asset.guid.IsValid())
                    continue;

                binding.reference = {
                    NLS::Core::Assets::AssetId(asset.guid),
                    asset.filePath
                };
            }

            if (const auto* entry = runtimeAssets.Resolve(binding.reference))
            {
                binding.resolved = entry->artifactType == NLS::Core::Assets::ArtifactType::Material;
                binding.artifactType = entry->artifactType;
                binding.loaderId = entry->loaderId;
                binding.artifactPath = entry->artifactPath;
            }
            bindings.push_back(std::move(binding));
        }
    }
    return bindings;
}
}
