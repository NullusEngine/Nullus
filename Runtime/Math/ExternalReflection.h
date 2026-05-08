#pragma once

#include "Reflection/ExternalReflectionRegistration.h"
#include "Quaternion.h"
#include "Vector3.h"
#include "Vector4.h"

namespace NLS::Maths
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Vector3)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Quaternion)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Vector4)

inline void RegisterMathExternalReflection(
    NLS::meta::ReflectionDatabase& db,
    NLS::meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Maths::Vector3)
        NLS_META_EXTERNAL_FIELD(float, x);
        NLS_META_EXTERNAL_FIELD(float, y);
        NLS_META_EXTERNAL_FIELD(float, z);
        NLS_META_EXTERNAL_METHOD(
            "Length",
            static_cast<float(NLS::Maths::Vector3::*)() const>(&NLS::Maths::Vector3::Length));
        NLS_META_EXTERNAL_METHOD("LengthSquared", &NLS::Maths::Vector3::LengthSquared);
        NLS_META_EXTERNAL_METHOD("GetMaxElement", &NLS::Maths::Vector3::GetMaxElement);
        NLS_META_EXTERNAL_METHOD("GetAbsMaxElement", &NLS::Maths::Vector3::GetAbsMaxElement);
        NLS_META_EXTERNAL_METHOD("Normalised", &NLS::Maths::Vector3::Normalised);
        NLS_META_EXTERNAL_METHOD("Normalise", &NLS::Maths::Vector3::Normalise);
        NLS_META_EXTERNAL_STATIC_METHOD(
            "Dot",
            static_cast<float(*)(const NLS::Maths::Vector3&, const NLS::Maths::Vector3&)>(&NLS::Maths::Vector3::Dot));
        NLS_META_EXTERNAL_STATIC_METHOD(
            "Distance",
            static_cast<float(*)(const NLS::Maths::Vector3&, const NLS::Maths::Vector3&)>(&NLS::Maths::Vector3::Distance));
        NLS_META_EXTERNAL_STATIC_METHOD(
            "Cross",
            static_cast<NLS::Maths::Vector3(*)(const NLS::Maths::Vector3&, const NLS::Maths::Vector3&)>(&NLS::Maths::Vector3::Cross));
        NLS_META_EXTERNAL_STATIC_METHOD(
            "Normalize",
            static_cast<NLS::Maths::Vector3(*)(const NLS::Maths::Vector3&)>(&NLS::Maths::Vector3::Normalize));
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Maths::Quaternion)
        NLS_META_EXTERNAL_FIELD(float, x);
        NLS_META_EXTERNAL_FIELD(float, y);
        NLS_META_EXTERNAL_FIELD(float, z);
        NLS_META_EXTERNAL_FIELD(float, w);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Maths::Vector4)
        NLS_META_EXTERNAL_FIELD(float, x);
        NLS_META_EXTERNAL_FIELD(float, y);
        NLS_META_EXTERNAL_FIELD(float, z);
        NLS_META_EXTERNAL_FIELD(float, w);
    NLS_META_EXTERNAL_END();
}
} // namespace NLS::Maths

NLS_META_EXTERNAL_MODULE(NLS::Maths::RegisterMathExternalReflection)
