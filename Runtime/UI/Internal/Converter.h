#pragma once

#include <Vector2.h>
#include <Vector4.h>

#include "UI/UIDef.h"
#include "ImGui/imgui.h"
#include "Color.h"

namespace NLS::UI::Internal
{
/**
 * Handles imgui conversion to/from overload types
 */
class NLS_UI_API Converter
{
public:
    /**
     * Disabled constructor
     */
    Converter() = delete;

    /**
     * Convert the given Color to ImVec4
     * @param p_value
     */
    static ImVec4 ToImVec4(const Maths::Color& p_value);

    /**
     * Convert the given ImVec4 to Color
     * @param p_value
     */
    static Maths::Color ToColor(const ImVec4& p_value);

    /**
     * Convert the given FVector2 to ImVec2
     * @param p_value
     */
    static ImVec2 ToImVec2(const Maths::Vector2& p_value);

    /**
     * Convert the given ImVec2 to FVector2
     * @param p_value
     */
    static Maths::Vector2 ToFVector2(const ImVec2& p_value);
};
} // namespace NLS::UI::Internal