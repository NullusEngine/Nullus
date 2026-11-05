#pragma once

#include "SceneSystem/ScenePhysicsSettings.h"
#include "Serialize/ObjectGraphDocument.h"
#include <cmath>
#include <limits>
#include <unordered_set>

namespace NLS::Engine::Serialize
{
inline PropertyValue WriteScenePhysicsSettings(const SceneSystem::ScenePhysicsSettings& settings)
{
    return PropertyValue::Object({
        {"version", PropertyValue::Integer(1)},
        {"gravity3D", PropertyValue::Array({PropertyValue::Number(settings.gravity3D.x),
            PropertyValue::Number(settings.gravity3D.y), PropertyValue::Number(settings.gravity3D.z)})},
        {"gravity2D", PropertyValue::Array({PropertyValue::Number(settings.gravity2D.x), PropertyValue::Number(settings.gravity2D.y)})},
        {"solverSubsteps2D", PropertyValue::Integer(settings.solverSubsteps2D)},
        {"workerCount3D", PropertyValue::Integer(settings.workerCount3D)},
        {"workerCount2D", PropertyValue::Integer(settings.workerCount2D)},
        {"interpolate", PropertyValue::Bool(settings.interpolate)}
    });
}

inline bool ReadScenePhysicsSettings(const PropertyValue& value, SceneSystem::ScenePhysicsSettings& output, std::string& error)
{
    using Kind = PropertyValue::Kind;
    if (value.GetKind() != Kind::Object) { error = "Scene physics settings must be an object."; return false; }
    std::unordered_set<std::string> names;
    for (const auto& [name, _] : value.GetObject())
        if (!names.insert(name).second) { error = "Duplicate scene physics field: " + name; return false; }
    const auto find = [&](const char* name) -> const PropertyValue*
    {
        for (const auto& entry : value.GetObject()) if (entry.first == name) return &entry.second;
        return nullptr;
    };
    if (const auto* version = find("version"); version && (version->GetKind() != Kind::Integer || version->GetInteger() != 1))
    { error = "Unsupported scene physics settings version (expected 1)."; return false; }
    SceneSystem::ScenePhysicsSettings settings;
    const auto vector = [&](const char* name, float* outputValues, size_t count)
    {
        const auto* field = find(name); if (!field) return true;
        if (field->GetKind() != Kind::Array || field->GetArray().size() != count)
        { error = std::string("Invalid scene physics vector: ") + name; return false; }
        for (size_t i = 0; i < count; ++i)
        {
            const auto& item = field->GetArray()[i];
            if (item.GetKind() != Kind::Integer && item.GetKind() != Kind::Number)
            { error = std::string("Non-numeric scene physics vector: ") + name; return false; }
            const double number = item.GetKind() == Kind::Integer ? static_cast<double>(item.GetInteger()) : item.GetNumber();
            if (!std::isfinite(number) || std::abs(number) > (std::numeric_limits<float>::max)())
            { error = std::string("Non-finite scene physics vector: ") + name; return false; }
            outputValues[i] = static_cast<float>(number);
        }
        return true;
    };
    float gravity3D[]{settings.gravity3D.x, settings.gravity3D.y, settings.gravity3D.z};
    float gravity2D[]{settings.gravity2D.x, settings.gravity2D.y};
    if (!vector("gravity3D", gravity3D, 3) || !vector("gravity2D", gravity2D, 2)) return false;
    settings.gravity3D = {gravity3D[0], gravity3D[1], gravity3D[2]}; settings.gravity2D = {gravity2D[0], gravity2D[1]};
    const auto integer = [&](const char* name, uint32_t& destination)
    {
        const auto* field = find(name); if (!field) return true;
        if (field->GetKind() != Kind::Integer || field->GetInteger() < 0 || field->GetInteger() > UINT32_MAX)
        { error = std::string("Invalid scene physics integer: ") + name; return false; }
        destination = static_cast<uint32_t>(field->GetInteger()); return true;
    };
    if (!integer("solverSubsteps2D", settings.solverSubsteps2D) || !integer("workerCount3D", settings.workerCount3D) ||
        !integer("workerCount2D", settings.workerCount2D)) return false;
    if (const auto* field = find("interpolate"))
    {
        if (field->GetKind() != Kind::Bool) { error = "Scene physics interpolate must be a boolean."; return false; }
        settings.interpolate = field->GetBool();
    }
    if (!settings.Validate(error)) return false;
    output = settings; error.clear(); return true;
}
}
