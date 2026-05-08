#pragma once

#include <unordered_map>

#include "GameObject.h"
#include "Serialize/ObjectGraphDocument.h"

namespace NLS::Engine::Serialize
{
    struct PrefabDocument
    {
        ObjectGraphDocument graph;
        AssetReferenceValue basePrefab;
    };

    struct PrefabInstantiationResult
    {
        GameObject* root = nullptr;
        std::unordered_map<ObjectId, ObjectId> sourceToInstance;
    };
}
