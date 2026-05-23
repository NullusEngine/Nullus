#pragma once

#include "Reflection/ExternalReflectionRegistration.h"
#include "Engine/LayerMask.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/PPtr.h"
#include "Serialize/PPtrResourceTypes.h"
#include "Rendering/ExternalReflection.h"

namespace NLS::Engine
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::LayerMask)
}

namespace NLS::Engine::Serialize
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::FileType)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::ObjectIdentifier)
#define NLS_ENGINE_SERIALIZE_DECLARE_PPTR_EXTERNAL_TYPE(type, label, artifactType, subAssetPrefix) \
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::PPtr<type>)
NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_ENGINE_SERIALIZE_DECLARE_PPTR_EXTERNAL_TYPE)
#undef NLS_ENGINE_SERIALIZE_DECLARE_PPTR_EXTERNAL_TYPE

inline void RegisterSerializeExternalReflection(
    NLS::meta::ReflectionDatabase& db,
    NLS::meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Engine::LayerMask)
        NLS_META_EXTERNAL_FIELD_NAMED(uint32_t, "mask", &NLS::Engine::LayerMask::GetMask, &NLS::Engine::LayerMask::SetMask);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::FileType)
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::ObjectIdentifier)
        NLS_META_EXTERNAL_FIELD(NLS::Guid, guid);
        NLS_META_EXTERNAL_FIELD(int64_t, localIdentifierInFile);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Serialize::FileType, fileType);
        NLS_META_EXTERNAL_FIELD(std::string, filePath);
    NLS_META_EXTERNAL_END();

#define NLS_ENGINE_SERIALIZE_REGISTER_PPTR_EXTERNAL_TYPE(type, label, artifactType, subAssetPrefix) \
    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::PPtr<type>)       \
    NLS_META_EXTERNAL_END();
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_ENGINE_SERIALIZE_REGISTER_PPTR_EXTERNAL_TYPE)
#undef NLS_ENGINE_SERIALIZE_REGISTER_PPTR_EXTERNAL_TYPE
}
} // namespace NLS::Engine::Serialize

NLS_META_EXTERNAL_MODULE(NLS::Engine::Serialize::RegisterSerializeExternalReflection)
