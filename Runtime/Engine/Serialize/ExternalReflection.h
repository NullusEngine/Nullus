#pragma once

#include "Reflection/ExternalReflectionRegistration.h"
#include "Components/Component.h"
#include "Engine/LayerMask.h"
#include "GameObject.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/PPtr.h"
#include "Serialize/PPtrResourceTypes.h"
#include "Serialize/SerializedSpecialTypes.h"
#include "UI/UIDocumentReference.h"
#include "Rendering/ExternalReflection.h"

namespace NLS::Engine
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::LayerMask)
}

namespace NLS::Engine::Serialize
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::FileType)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::ObjectIdentifier)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::PPtr<NLS::Engine::Components::Component>)
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

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::UI::UIDocumentReference)
        NLS_META_EXTERNAL_FIELD_NAMED(std::string, "assetId", &NLS::Engine::UI::UIDocumentReference::GetAssetIdString, &NLS::Engine::UI::UIDocumentReference::SetAssetIdString);
        NLS_META_EXTERNAL_FIELD_NAMED(std::string, "resourcePath", &NLS::Engine::UI::UIDocumentReference::GetResourcePath, &NLS::Engine::UI::UIDocumentReference::SetResourcePath);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::FileType)
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::ObjectIdentifier)
        NLS_META_EXTERNAL_FIELD(NLS::Guid, guid);
        NLS_META_EXTERNAL_FIELD(int64_t, localIdentifierInFile);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Serialize::FileType, fileType);
        NLS_META_EXTERNAL_FIELD(std::string, filePath);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>)
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::PPtr<NLS::Engine::Components::Component>)
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::AnimationCurveKeyframe)
        NLS_META_EXTERNAL_FIELD(float, time);
        NLS_META_EXTERNAL_FIELD(float, value);
        NLS_META_EXTERNAL_FIELD(float, inTangent);
        NLS_META_EXTERNAL_FIELD(float, outTangent);
        NLS_META_EXTERNAL_FIELD(float, inWeight);
        NLS_META_EXTERNAL_FIELD(float, outWeight);
        NLS_META_EXTERNAL_FIELD(int32_t, weightedMode);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::AnimationCurve)
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Engine::Serialize::AnimationCurveKeyframe>, keys);
        NLS_META_EXTERNAL_FIELD(int32_t, preWrapMode);
        NLS_META_EXTERNAL_FIELD(int32_t, postWrapMode);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::GradientColorKey)
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Color, color);
        NLS_META_EXTERNAL_FIELD(float, time);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::GradientAlphaKey)
        NLS_META_EXTERNAL_FIELD(float, alpha);
        NLS_META_EXTERNAL_FIELD(float, time);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::Gradient)
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Engine::Serialize::GradientColorKey>, colorKeys);
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Engine::Serialize::GradientAlphaKey>, alphaKeys);
        NLS_META_EXTERNAL_FIELD(int32_t, mode);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::ExposedReference)
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>, defaultValue);
        NLS_META_EXTERNAL_FIELD(std::string, exposedName);
    NLS_META_EXTERNAL_END();

#define NLS_ENGINE_SERIALIZE_REGISTER_PPTR_EXTERNAL_TYPE(type, label, artifactType, subAssetPrefix) \
    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::PPtr<type>)       \
    NLS_META_EXTERNAL_END();
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_ENGINE_SERIALIZE_REGISTER_PPTR_EXTERNAL_TYPE)
#undef NLS_ENGINE_SERIALIZE_REGISTER_PPTR_EXTERNAL_TYPE
}
} // namespace NLS::Engine::Serialize

NLS_META_EXTERNAL_MODULE(NLS::Engine::Serialize::RegisterSerializeExternalReflection)
