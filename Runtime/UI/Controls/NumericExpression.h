#pragma once
#include "UI/UIDef.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace NLS::UI::Controls
{
struct ExpressionValue
{
    long double number = 0;
    uint64_t magnitude = 0;
    bool negative = false, exactInteger = false;
    std::string error;
    explicit operator bool() const
    {
        return error.empty();
    }
};

NLS_UI_API ExpressionValue EvaluateExpression(std::string_view text);

template<class T>
bool EvaluateExpressionAs(std::string_view text, T& output, std::string& error)
{
    const auto value = EvaluateExpression(text);
    error = value.error;
    if (!value)
        return false;
    if constexpr (std::is_integral_v<T>)
    {
        if (!value.exactInteger)
        {
            error = "Expression must produce an exactly representable integer";
            return false;
        }
        const uint64_t positiveLimit = static_cast<uint64_t>((std::numeric_limits<T>::max)());
        const uint64_t negativeLimit = std::is_signed_v<T> ? positiveLimit + 1ull : 0ull;
        if ((!value.negative && value.magnitude > positiveLimit) || (value.negative && value.magnitude > negativeLimit))
        {
            error = "Integer overflow";
            return false;
        }
        if (value.negative && value.magnitude != 0)
        {
            if constexpr (std::is_signed_v<T>)
                output = value.magnitude == negativeLimit ? (std::numeric_limits<T>::lowest)() : static_cast<T>(-static_cast<int64_t>(value.magnitude));
            else
            {
                error = "Unsigned value cannot be negative";
                return false;
            }
        }
        else
            output = static_cast<T>(value.magnitude);
    }
    else
    {
        if (!std::isfinite(value.number) || std::abs(value.number) > (std::numeric_limits<T>::max)())
        {
            error = "Floating point overflow";
            return false;
        }
        output = static_cast<T>(value.number);
    }
    return true;
}
} // namespace NLS::UI::Controls
