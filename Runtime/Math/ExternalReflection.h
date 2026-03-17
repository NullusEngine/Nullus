#pragma once

#include "Reflection/Macros.h"
#include "Quaternion.h"
#include "Vector3.h"

namespace NLS::Maths
{
MetaExternal(NLS::Maths::Vector3)

REFLECT_EXTERNAL(
    NLS::Maths::Vector3,
    Fields(
        REFLECT_FIELD(float, x),
        REFLECT_FIELD(float, y),
        REFLECT_FIELD(float, z)
    ),
    Methods(
        REFLECT_METHOD_EX(Length, static_cast<float(NLS::Maths::Vector3::*)() const>(&NLS::Maths::Vector3::Length)),
        REFLECT_METHOD(LengthSquared),
        REFLECT_METHOD(GetMaxElement),
        REFLECT_METHOD(GetAbsMaxElement),
        REFLECT_METHOD(Normalised),
        REFLECT_METHOD(Normalise)
    ),
    StaticMethods(
        REFLECT_STATIC_METHOD(Dot, static_cast<float(*)(const NLS::Maths::Vector3&, const NLS::Maths::Vector3&)>(&NLS::Maths::Vector3::Dot)),
        REFLECT_STATIC_METHOD(Distance, static_cast<float(*)(const NLS::Maths::Vector3&, const NLS::Maths::Vector3&)>(&NLS::Maths::Vector3::Distance)),
        REFLECT_STATIC_METHOD(Cross, static_cast<NLS::Maths::Vector3(*)(const NLS::Maths::Vector3&, const NLS::Maths::Vector3&)>(&NLS::Maths::Vector3::Cross)),
        REFLECT_STATIC_METHOD(Normalize, static_cast<NLS::Maths::Vector3(*)(const NLS::Maths::Vector3&)>(&NLS::Maths::Vector3::Normalize))
    )
)

MetaExternal(NLS::Maths::Quaternion)

REFLECT_EXTERNAL(
    NLS::Maths::Quaternion,
    Fields(
        REFLECT_FIELD(float, x),
        REFLECT_FIELD(float, y),
        REFLECT_FIELD(float, z),
        REFLECT_FIELD(float, w)
    )
)
} // namespace NLS::Maths
