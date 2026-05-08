#pragma once

#include <cstdint>
#include <string>

#include "Reflection/Array.h"
#include "Reflection/ExternalReflectionRegistration.h"

namespace NLS::Engine::Serialize
{
struct SerializedComponentData
{
    std::string type;
    std::string data;
};

struct SerializedActorData
{
    std::string name;
    std::string tag;
    bool active = true;
    int worldID = 0;
    int64_t parent = 0;
    NLS::Array<SerializedComponentData> components;
};

struct SerializedSceneData
{
    int version = 1;
    NLS::Array<SerializedActorData> actors;
};

NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::SerializedComponentData)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::SerializedActorData)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::SerializedSceneData)

inline void RegisterSceneSerializationExternalReflection(
    NLS::meta::ReflectionDatabase& db,
    NLS::meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::SerializedComponentData)
        NLS_META_EXTERNAL_FIELD(std::string, type);
        NLS_META_EXTERNAL_FIELD(std::string, data);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::SerializedActorData)
        NLS_META_EXTERNAL_FIELD(std::string, name);
        NLS_META_EXTERNAL_FIELD(std::string, tag);
        NLS_META_EXTERNAL_FIELD(bool, active);
        NLS_META_EXTERNAL_FIELD(int, worldID);
        NLS_META_EXTERNAL_FIELD(int64_t, parent);
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Engine::Serialize::SerializedComponentData>, components);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::SerializedSceneData)
        NLS_META_EXTERNAL_FIELD(int, version);
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Engine::Serialize::SerializedActorData>, actors);
    NLS_META_EXTERNAL_END();
}
} // namespace NLS::Engine::Serialize

NLS_META_EXTERNAL_MODULE(NLS::Engine::Serialize::RegisterSceneSerializationExternalReflection)
