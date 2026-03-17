#pragma once

#include <cstdint>
#include <string>

#include "Reflection/Array.h"
#include "Reflection/Macros.h"

namespace NLS
{
namespace Engine
{
namespace Serialize
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
} // namespace Serialize
} // namespace Engine
} // namespace NLS

MetaExternal(NLS::Engine::Serialize::SerializedComponentData)
MetaExternal(NLS::Engine::Serialize::SerializedActorData)
MetaExternal(NLS::Engine::Serialize::SerializedSceneData)

REFLECT_EXTERNAL(
    NLS::Engine::Serialize::SerializedComponentData,
    Fields(
        REFLECT_FIELD(std::string, type),
        REFLECT_FIELD(std::string, data)
    )
)

REFLECT_EXTERNAL(
    NLS::Engine::Serialize::SerializedActorData,
    Fields(
        REFLECT_FIELD(std::string, name),
        REFLECT_FIELD(std::string, tag),
        REFLECT_FIELD(bool, active),
        REFLECT_FIELD(int, worldID),
        REFLECT_FIELD(int64_t, parent),
        REFLECT_FIELD(NLS::Array<NLS::Engine::Serialize::SerializedComponentData>, components)
    )
)

REFLECT_EXTERNAL(
    NLS::Engine::Serialize::SerializedSceneData,
    Fields(
        REFLECT_FIELD(int, version),
        REFLECT_FIELD(NLS::Array<NLS::Engine::Serialize::SerializedActorData>, actors)
    )
)
