#pragma once
#include "UI/Controls/Control.h"
#include "UI/Controls/NumericExpression.h"

namespace NLS::UI::Controls
{
struct ScalarOptions
{
    const void* minimum = nullptr;
    const void* maximum = nullptr;
    const char* format = nullptr;
    float speed = .01f;
    bool drag = false, slider = false, expression = true;
    bool vertical = false;
    ImVec2 size{};
    ImGuiSliderFlags flags = 0;
    const void* step = nullptr;
    const void* fastStep = nullptr;
    ImGuiInputTextFlags textFlags = 0;
};
NLS_UI_API ControlResult Scalar(const char* id, ImGuiDataType type, void* value, const ScalarOptions& scalar = {},
                                const ControlOptions& options = {});

template<class T>
constexpr ImGuiDataType ScalarType()
{
    if constexpr (std::is_same_v<T, float>)
        return ImGuiDataType_Float;
    if constexpr (std::is_same_v<T, double>)
        return ImGuiDataType_Double;
    if constexpr (std::is_integral_v<T>)
    {
        if constexpr (sizeof(T) == 1)
            return std::is_signed_v<T> ? ImGuiDataType_S8 : ImGuiDataType_U8;
        if constexpr (sizeof(T) == 2)
            return std::is_signed_v<T> ? ImGuiDataType_S16 : ImGuiDataType_U16;
        if constexpr (sizeof(T) == 4)
            return std::is_signed_v<T> ? ImGuiDataType_S32 : ImGuiDataType_U32;
        if constexpr (sizeof(T) == 8)
            return std::is_signed_v<T> ? ImGuiDataType_S64 : ImGuiDataType_U64;
    }
    return ImGuiDataType_COUNT;
}
template<class T>
ControlResult Numeric(const char* id, T& value, const ScalarOptions& scalar = {}, const ControlOptions& options = {})
{
    return Scalar(id, ScalarType<T>(), &value, scalar, options);
}
} // namespace NLS::UI::Controls
