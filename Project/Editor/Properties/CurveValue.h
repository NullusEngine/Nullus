#pragma once
#include <Math/ExternalReflection.h>
#include <Reflection/Array.h>
#include <UI/Controls/CurveControl.h>

namespace NLS::Editor::Properties
{
// Editor adapter for multi-channel domain curves. Assets retain their original
// curve format; this value only crosses the shared property editing boundary.
struct CurveValueKey
{
    double time = 0;
    Maths::Vector4 value{}, inTangent{}, outTangent{};
    int32_t interpolation = 0;
};
struct CurveValue
{
    NLS::Array<CurveValueKey> keys;
    uint32_t channels = 1;
};
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Properties::CurveValueKey)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Properties::CurveValue)
inline void RegisterPropertyCurveReflection(meta::ReflectionDatabase& db, meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Properties::CurveValueKey)
    NLS_META_EXTERNAL_FIELD(double, time);
    NLS_META_EXTERNAL_FIELD(Maths::Vector4, value);
    NLS_META_EXTERNAL_FIELD(Maths::Vector4, inTangent);
    NLS_META_EXTERNAL_FIELD(Maths::Vector4, outTangent);
    NLS_META_EXTERNAL_FIELD(int32_t, interpolation);
    NLS_META_EXTERNAL_END();
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Properties::CurveValue)
    NLS_META_EXTERNAL_FIELD(NLS::Array<CurveValueKey>, keys);
    NLS_META_EXTERNAL_FIELD(uint32_t, channels);
    NLS_META_EXTERNAL_END();
}
inline UI::Controls::CurveData ToControlCurve(const CurveValue& value)
{
    UI::Controls::CurveData result;
    result.channels = value.channels;
    for (const auto& key : value.keys)
    {
        UI::Controls::CurveKey k;
        k.time = key.time;
        k.value = {key.value.x, key.value.y, key.value.z, key.value.w};
        k.inTangent = {key.inTangent.x, key.inTangent.y, key.inTangent.z, key.inTangent.w};
        k.outTangent = {key.outTangent.x, key.outTangent.y, key.outTangent.z, key.outTangent.w};
        k.interpolation = key.interpolation;
        result.keys.push_back(k);
    }
    return result;
}
inline CurveValue FromControlCurve(const UI::Controls::CurveData& data)
{
    CurveValue value;
    value.channels = static_cast<uint32_t>(data.channels);
    for (const auto& k : data.keys)
        value.keys.push_back({k.time,
                              {k.value[0], k.value[1], k.value[2], k.value[3]},
                              {k.inTangent[0], k.inTangent[1], k.inTangent[2], k.inTangent[3]},
                              {k.outTangent[0], k.outTangent[1], k.outTangent[2], k.outTangent[3]},
                              k.interpolation});
    return value;
}
} // namespace NLS::Editor::Properties
NLS_META_EXTERNAL_MODULE(NLS::Editor::Properties::RegisterPropertyCurveReflection)
