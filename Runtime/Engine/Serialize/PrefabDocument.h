#pragma once

#include <unordered_map>

#include "GameObject.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/SerializationDiagnostic.h"

namespace NLS::Engine::Serialize
{
    struct PrefabDocument
    {
        ObjectGraphDocument graph;
        ObjectIdentifier basePrefab;
    };

    struct PrefabInstantiationResult
    {
        GameObject* root = nullptr;
        std::unordered_map<ObjectId, ObjectId> sourceToInstance;
        std::unordered_map<GameObject*, ObjectId> sourceByInstanceObject;
        SerializationDiagnosticList diagnostics;
    };
}
