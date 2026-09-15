#pragma once

#include <cstdint>
#include <string>

#include "GameObject.h"
#include "Math/Color.h"
#include "Reflection/Array.h"
#include "Reflection/ExternalReflectionRegistration.h"
#include "Serialize/PPtr.h"

namespace NLS::Engine::Serialize
{
// Deterministic authoring representation of the external editor's Keyframe. Runtime spline
// caches are deliberately excluded from serialized data.
struct NLS_ENGINE_API AnimationCurveKeyframe
{
    float time = 0.0f;
    float value = 0.0f;
    float inTangent = 0.0f;
    float outTangent = 0.0f;
    float inWeight = 1.0f;
    float outWeight = 1.0f;
    int32_t weightedMode = 0;

    friend bool operator==(const AnimationCurveKeyframe&, const AnimationCurveKeyframe&) = default;
};

struct NLS_ENGINE_API AnimationCurve
{
    NLS::Array<AnimationCurveKeyframe> keys;
    int32_t preWrapMode = 0;
    int32_t postWrapMode = 0;

    friend bool operator==(const AnimationCurve&, const AnimationCurve&) = default;
};

struct NLS_ENGINE_API GradientColorKey
{
    NLS::Maths::Color color;
    float time = 0.0f;

    friend bool operator==(const GradientColorKey& left, const GradientColorKey& right)
    {
        return left.color.r == right.color.r && left.color.g == right.color.g &&
            left.color.b == right.color.b && left.color.a == right.color.a &&
            left.time == right.time;
    }
};

struct NLS_ENGINE_API GradientAlphaKey
{
    float alpha = 1.0f;
    float time = 0.0f;

    friend bool operator==(const GradientAlphaKey&, const GradientAlphaKey&) = default;
};

struct NLS_ENGINE_API Gradient
{
    NLS::Array<GradientColorKey> colorKeys;
    NLS::Array<GradientAlphaKey> alphaKeys;
    int32_t mode = 0;

    friend bool operator==(const Gradient&, const Gradient&) = default;
};

// the external editor's ExposedReference<T> carries an object default and a resolver key.
// GameObject is the canonical native carrier; the managed side exposes a
// generic wrapper over the same fields.
struct NLS_ENGINE_API ExposedReference
{
    PPtr<NLS::Engine::GameObject> defaultValue;
    std::string exposedName;
};

NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::AnimationCurveKeyframe)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::AnimationCurve)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::GradientColorKey)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::GradientAlphaKey)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::Gradient)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::ExposedReference)
} // namespace NLS::Engine::Serialize
