#pragma once
#include <Math/ExternalReflection.h>
#include <Reflection/Array.h>
#include "Reflection/Macros.h"
#if !defined(__REFLECTION_PARSER__)
#include "Project/Editor/Properties/CurveValue.generated.h"
#endif

namespace NLS::UI::Controls { struct CurveData; }

namespace NLS::Editor::Properties
{
// Editor adapter for multi-channel domain curves. Assets retain their original
// curve format; this value only crosses the shared property editing boundary.
STRUCT(CurveValueKey)
{
    GENERATED_BODY()
    PROPERTY()
    double time = 0;
    PROPERTY() Maths::Vector4 value{};
    PROPERTY() Maths::Vector4 inTangent{};
    PROPERTY() Maths::Vector4 outTangent{};
    PROPERTY()
    int32_t interpolation = 0;
};
STRUCT(CurveValue)
{
    GENERATED_BODY()
    PROPERTY()
    NLS::Array<CurveValueKey> keys;
    PROPERTY()
    uint32_t channels = 1;
};
UI::Controls::CurveData ToControlCurve(const CurveValue& value);
CurveValue FromControlCurve(const UI::Controls::CurveData& data);
} // namespace NLS::Editor::Properties
