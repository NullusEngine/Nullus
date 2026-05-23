#pragma once

#include <string>
#include <type_traits>

#include "Assets/AssetId.h"
#include "Core/ServiceLocator.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/PPtr.h"

namespace NLS::Engine::Serialize
{
    inline const Assets::RuntimeAssetManifestEntry* ResolveRuntimeAssetReferenceEntry(
        const ObjectIdentifier& reference)
    {
        if (!reference.IsAsset() ||
            !Core::ServiceLocator::Contains<Assets::RuntimeAssetDatabase>())
        {
            return nullptr;
        }

        const auto& runtimeAssets = NLS_SERVICE(Assets::RuntimeAssetDatabase);
        const auto assetId = NLS::Core::Assets::AssetId(reference.guid);
        return runtimeAssets.ResolveByLocalIdentifierInFile(assetId, reference.localIdentifierInFile);
    }

    inline std::string ResolveRuntimeAssetReferenceSubAssetKey(const ObjectIdentifier& reference)
    {
        const auto* entry = ResolveRuntimeAssetReferenceEntry(reference);
        return entry != nullptr ? entry->subAssetKey : std::string {};
    }

    inline std::string ResolveAssetReferencePath(const ObjectIdentifier& reference)
    {
        if (!reference.IsAsset())
            return {};

        if (Core::ServiceLocator::Contains<Assets::RuntimeAssetDatabase>())
        {
            const auto* entry = ResolveRuntimeAssetReferenceEntry(reference);
            if (entry == nullptr)
                return reference.filePath;

            if (!entry->artifactPath.empty())
            {
                auto updatedReference = reference;
                updatedReference.filePath = entry->artifactPath;
                PersistentManager::Instance().ObjectIdentifierToInstanceID(updatedReference);
            }
            return entry->artifactPath;
        }

        return reference.filePath;
    }

    template <typename T>
    bool BindResolvedObjectReference(T& object, PPtr<T>& reference)
        requires Detail::IsCompleteObjectTargetV<T>
    {
        ObjectIdentifier identifier;
        if (!PersistentManager::Instance().InstanceIDToObjectIdentifier(reference.GetInstanceID(), identifier))
            return false;

        const auto instanceID = PersistentManager::Instance().BindObjectIdentifier(object, identifier);
        if (instanceID == InstanceID_None)
            return false;

        reference.SetInstanceID(instanceID);
        return true;
    }
}
