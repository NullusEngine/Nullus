#include "Properties/CurveValue.h"
#include <UI/Controls/CurveControl.h>

namespace NLS::Editor::Properties
{
UI::Controls::CurveData ToControlCurve(const CurveValue& value)
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

CurveValue FromControlCurve(const UI::Controls::CurveData& data)
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
}
