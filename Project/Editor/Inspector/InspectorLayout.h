#pragma once

#include <cstddef>
#include <vector>

namespace NLS::Editor::Inspector
{
inline constexpr float kInspectorMinimumLabelLogicalWidth = 96.0f;
inline constexpr float kInspectorMaximumLabelLogicalWidth = 220.0f;
inline constexpr float kInspectorCompactLogicalWidth = 360.0f;
inline constexpr float kInspectorMinimumActionLogicalWidth = 48.0f;
inline constexpr float kInspectorMinimumComponentInputLogicalWidth = 48.0f;

struct InspectorLayoutRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct InspectorPropertiesLayout
{
    bool compact = false;
    float labelLogicalWidth = 0.0f;
    float valueLogicalWidth = 0.0f;
};

struct InspectorActionLayout
{
    bool stacked = false;
    std::vector<InspectorLayoutRect> buttons;
};

struct InspectorMultiFieldLayout
{
    bool stacked = false;
    float groupWidth = 0.0f;
    float inputWidth = 0.0f;
};

[[nodiscard]] InspectorPropertiesLayout BuildInspectorPropertiesLayout(
    float availableWidthPixels,
    float uiScale,
    float requestedLabelLogicalWidth = 0.0f);

[[nodiscard]] InspectorActionLayout BuildInspectorActionLayout(
    float availableWidthPixels,
    float uiScale,
    const std::vector<float>& labelWidthsPixels,
    float itemSpacingPixels,
    float horizontalPaddingPixels);

[[nodiscard]] InspectorMultiFieldLayout BuildInspectorMultiFieldLayout(
    float availableWidthPixels,
    float uiScale,
    size_t componentCount,
    float itemSpacingPixels,
    float componentLabelWidthPixels);
} // namespace NLS::Editor::Inspector
