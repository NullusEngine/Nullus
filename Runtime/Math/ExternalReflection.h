#pragma once

#include "Reflection/ExternalReflectionRegistration.h"
#include "Color.h"
#include "Bounds.h"
#include "BoundsInt.h"
#include "Quaternion.h"
#include "Rect.h"
#include "RectInt.h"
#include "Vector2.h"
#include "Vector2Int.h"
#include "Vector3.h"
#include "Vector3Int.h"
#include "Vector4.h"

namespace NLS::Maths
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Vector2)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Vector3)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Quaternion)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Vector4)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Color)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Rect)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Vector2Int)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Vector3Int)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::RectInt)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::Bounds)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Maths::BoundsInt)

inline void RegisterMathExternalReflection(
    NLS::meta::ReflectionDatabase& db,
    NLS::meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Maths::Vector2)
        NLS_META_EXTERNAL_FIELD(float, x);
        NLS_META_EXTERNAL_FIELD(float, y);
    NLS_META_EXTERNAL_END();

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

    NLS_META_EXTERNAL_BEGIN(NLS::Maths::Color)
        NLS_META_EXTERNAL_FIELD(float, r);
        NLS_META_EXTERNAL_FIELD(float, g);
        NLS_META_EXTERNAL_FIELD(float, b);
        NLS_META_EXTERNAL_FIELD(float, a);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Maths::Rect)
        NLS_META_EXTERNAL_FIELD(float, x);
        NLS_META_EXTERNAL_FIELD(float, y);
        NLS_META_EXTERNAL_FIELD(float, width);
        NLS_META_EXTERNAL_FIELD(float, height);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Maths::Vector2Int)
        NLS_META_EXTERNAL_FIELD(int32_t, x);
        NLS_META_EXTERNAL_FIELD(int32_t, y);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Maths::Vector3Int)
        NLS_META_EXTERNAL_FIELD(int32_t, x);
        NLS_META_EXTERNAL_FIELD(int32_t, y);
        NLS_META_EXTERNAL_FIELD(int32_t, z);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Maths::RectInt)
        NLS_META_EXTERNAL_FIELD(int32_t, x);
        NLS_META_EXTERNAL_FIELD(int32_t, y);
        NLS_META_EXTERNAL_FIELD(int32_t, width);
        NLS_META_EXTERNAL_FIELD(int32_t, height);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Maths::Bounds)
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector3, center);
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector3, size);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Maths::BoundsInt)
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector3Int, position);
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector3Int, size);
    NLS_META_EXTERNAL_END();
}
} // namespace NLS::Maths

NLS_META_EXTERNAL_MODULE(NLS::Maths::RegisterMathExternalReflection)
