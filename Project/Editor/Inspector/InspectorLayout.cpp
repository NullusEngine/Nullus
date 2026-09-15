#include "Inspector/InspectorLayout.h"

#include <algorithm>

namespace NLS::Editor::Inspector
{
InspectorPropertiesLayout BuildInspectorPropertiesLayout(
    const float availableWidthPixels,
    const float uiScale,
    const float requestedLabelLogicalWidth)
{
    const float scale = (std::max)(uiScale, 0.01f);
    const float availableLogicalWidth = (std::max)(0.0f, availableWidthPixels) / scale;
    InspectorPropertiesLayout result;
    result.compact = availableLogicalWidth < kInspectorCompactLogicalWidth;
    if (result.compact)
    {
        result.labelLogicalWidth = availableLogicalWidth;
        result.valueLogicalWidth = availableLogicalWidth;
        return result;
    }

    const float requested = requestedLabelLogicalWidth > 0.0f
        ? requestedLabelLogicalWidth
        : availableLogicalWidth * 0.34f;
    result.labelLogicalWidth = std::clamp(
        requested,
        kInspectorMinimumLabelLogicalWidth,
        kInspectorMaximumLabelLogicalWidth);
    result.valueLogicalWidth = (std::max)(0.0f, availableLogicalWidth - result.labelLogicalWidth);
    return result;
}

InspectorActionLayout BuildInspectorActionLayout(
    const float availableWidthPixels,
    const float uiScale,
    const std::vector<float>& labelWidthsPixels,
    const float itemSpacingPixels,
    const float horizontalPaddingPixels)
{
    InspectorActionLayout result;
    if (labelWidthsPixels.empty())
        return result;

    const float scale = (std::max)(uiScale, 0.01f);
    const float availableWidth = (std::max)(0.0f, availableWidthPixels);
    const float spacing = (std::max)(0.0f, itemSpacingPixels);
    const float horizontalWidth = (std::max)(
        0.0f,
        (availableWidth - spacing * static_cast<float>(labelWidthsPixels.size() - 1u)) /
            static_cast<float>(labelWidthsPixels.size()));
    result.stacked = availableWidth / scale < kInspectorCompactLogicalWidth ||
        horizontalWidth < kInspectorMinimumActionLogicalWidth * scale ||
        std::any_of(labelWidthsPixels.begin(), labelWidthsPixels.end(), [&](const float labelWidth)
        {
            return labelWidth + horizontalPaddingPixels > horizontalWidth;
        });

    const float buttonWidth = result.stacked ? availableWidth : horizontalWidth;
    const float buttonHeight = 20.0f * scale;
    result.buttons.reserve(labelWidthsPixels.size());
    for (size_t index = 0u; index < labelWidthsPixels.size(); ++index)
    {
        result.buttons.push_back({
            result.stacked ? 0.0f : static_cast<float>(index) * (buttonWidth + spacing),
            result.stacked ? static_cast<float>(index) * (buttonHeight + spacing) : 0.0f,
            buttonWidth,
            buttonHeight});
    }
    return result;
}

InspectorMultiFieldLayout BuildInspectorMultiFieldLayout(
    const float availableWidthPixels,
    const float uiScale,
    const size_t componentCount,
    const float itemSpacingPixels,
    const float componentLabelWidthPixels)
{
    InspectorMultiFieldLayout result;
    if (componentCount == 0u)
        return result;

    const float scale = (std::max)(uiScale, 0.01f);
    const float availableWidth = (std::max)(0.0f, availableWidthPixels);
    const float spacing = (std::max)(0.0f, itemSpacingPixels);
    const float labelWidth = (std::max)(0.0f, componentLabelWidthPixels);
    const float horizontalGroupWidth = (std::max)(
        0.0f,
        (availableWidth - spacing * static_cast<float>(componentCount - 1u)) /
            static_cast<float>(componentCount));
    const float minimumInputWidth = kInspectorMinimumComponentInputLogicalWidth * scale;

    result.stacked = horizontalGroupWidth < labelWidth + minimumInputWidth;
    result.groupWidth = result.stacked ? availableWidth : horizontalGroupWidth;
    result.inputWidth = (std::max)(1.0f, result.groupWidth - labelWidth);
    return result;
}
} // namespace NLS::Editor::Inspector
