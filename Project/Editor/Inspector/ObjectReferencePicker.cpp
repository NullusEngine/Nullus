#include "Inspector/ObjectReferencePicker.h"

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

#include "Assets/EditorAssetDragPayload.h"
#include "UI/Widgets/Layout/Group.h"

namespace NLS::Editor::Inspector
{
namespace
{
std::string NormalizeSearchText(const std::string_view text)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (const auto raw : text)
    {
        const auto character = static_cast<unsigned char>(raw);
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

bool MatchesSearch(const InspectorObjectReferencePickerItem& item, const std::string_view normalizedSearch)
{
    if (normalizedSearch.empty())
        return true;
    const auto& source = item.searchText.empty() ? item.displayName : item.searchText;
    return NormalizeSearchText(source).find(normalizedSearch) != std::string::npos;
}

void CopyToBuffer(const std::string& value, std::vector<char>& buffer)
{
    buffer.assign((std::max)(size_t {128u}, value.size() + 1u), '\0');
    if (!value.empty())
        std::memcpy(buffer.data(), value.data(), value.size());
}

std::string TruncateUtf8ForWidth(const std::string& text, const float width)
{
    if (width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= width)
        return text;

    constexpr std::string_view suffix = "...";
    std::string result;
    result.reserve(text.size());
    for (size_t offset = 0u; offset < text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        const size_t length = (lead & 0x80u) == 0u ? 1u :
            (lead & 0xe0u) == 0xc0u ? 2u :
            (lead & 0xf0u) == 0xe0u ? 3u :
            (lead & 0xf8u) == 0xf0u ? 4u : 1u;
        if (offset + length > text.size())
            break;

        const auto candidate = result + text.substr(offset, length) + std::string(suffix);
        if (ImGui::CalcTextSize(candidate.c_str()).x > width)
            break;
        result += text.substr(offset, length);
        offset += length;
    }
    return result.empty() ? std::string(suffix) : result + std::string(suffix);
}

void DrawTooltipIfHovered(const std::string& tooltip)
{
    if (!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip.c_str());
}
} // namespace

bool DrawInspectorObjectReferencePicker(
    InspectorObjectReferencePickerState& state,
    const InspectorObjectReferencePickerConfig& config)
{
    const std::string id = config.id.empty() ? "ObjectReference" : config.id;
    const std::string searchKey = config.searchKey.empty() ? id : config.searchKey;
    const float scale = (std::max)(config.uiScale, 0.01f);
    const float controlWidth = 22.0f * scale;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float minimumInlineWidth = 48.0f * scale + controlWidth * 3.0f + spacing * 3.0f;
    const bool stackActions = availableWidth < minimumInlineWidth;
    const float displayWidth = (std::max)(
        stackActions ? 0.0f : 48.0f * scale,
        stackActions ? availableWidth : availableWidth - controlWidth * 3.0f - spacing * 3.0f);
    const float actionWidth = stackActions
        ? (std::max)(0.0f, (availableWidth - spacing * 2.0f) / 3.0f)
        : controlWidth;
    const std::string visibleDisplay = TruncateUtf8ForWidth(config.displayName, displayWidth - 8.0f * scale);
    const char* popupId = "##ObjectReferencePicker";
    bool changed = false;

    ImGui::PushID(id.c_str());
    if (ImGui::Button((visibleDisplay + "##ReferenceDisplay").c_str(), ImVec2(displayWidth, 0.0f)))
        ImGui::OpenPopup(popupId);
    DrawTooltipIfHovered(config.displayTooltip.empty() ? config.displayName : config.displayTooltip);

    if (config.assignAsset && ImGui::BeginDragDropTarget())
    {
        if (const auto* payload = ImGui::AcceptDragDropPayload(Assets::kEditorAssetDragPayloadType);
            payload != nullptr && payload->Data != nullptr &&
            payload->DataSize == static_cast<int>(sizeof(Assets::EditorAssetDragPayload)))
        {
            const auto assetPayload = *static_cast<const Assets::EditorAssetDragPayload*>(payload->Data);
            if ((!config.acceptsAsset || config.acceptsAsset(assetPayload)) && config.assignAsset(assetPayload))
                changed = true;
        }
        if (config.acceptsFile && config.assignFile)
        {
            constexpr const char* filePayloadType = "File";
            if (const auto* payload = ImGui::AcceptDragDropPayload(filePayloadType);
                payload != nullptr && payload->Data != nullptr &&
                payload->DataSize == static_cast<int>(sizeof(std::pair<std::string, UI::Widgets::Group*>)))
            {
                const auto& filePair = *static_cast<const std::pair<std::string, UI::Widgets::Group*>*>(
                    payload->Data);
                if (config.acceptsFile(filePair.first) && config.assignFile(filePair.first))
                    changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (stackActions)
        ImGui::Spacing();
    else
        ImGui::SameLine(0.0f, spacing);
    if (ImGui::Button("...", ImVec2(actionWidth, 0.0f)))
        ImGui::OpenPopup(popupId);
    DrawTooltipIfHovered("Select reference");

    ImGui::SameLine(0.0f, spacing);
    ImGui::BeginDisabled(!config.ping);
    if (ImGui::Button("P", ImVec2(actionWidth, 0.0f)) && config.ping)
        config.ping();
    ImGui::EndDisabled();
    DrawTooltipIfHovered(config.ping ? "Ping reference" : "No assigned reference to ping");

    ImGui::SameLine(0.0f, spacing);
    ImGui::BeginDisabled(!config.clear);
    if (ImGui::Button("X", ImVec2(actionWidth, 0.0f)) && config.clear && config.clear())
        changed = true;
    ImGui::EndDisabled();
    DrawTooltipIfHovered(config.clear ? "Clear reference" : "Reference cannot be cleared");

    if (ImGui::BeginPopup(popupId))
    {
        auto& searchText = state.searchTextByKey[searchKey];
        std::vector<char> searchBuffer;
        CopyToBuffer(searchText, searchBuffer);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##ReferenceSearch", "Search", searchBuffer.data(), searchBuffer.size()))
            searchText = searchBuffer.data();
        const auto normalizedSearch = NormalizeSearchText(searchText);
        ImGui::Separator();

        bool hasMatch = false;
        const auto items = config.buildItems ? config.buildItems() : std::vector<InspectorObjectReferencePickerItem> {};
        for (const auto& item : items)
        {
            if (!MatchesSearch(item, normalizedSearch))
                continue;
            hasMatch = true;
            if (ImGui::Selectable(item.displayName.c_str()) && item.select && item.select())
            {
                changed = true;
                ImGui::CloseCurrentPopup();
                break;
            }
        }
        if (!hasMatch)
            ImGui::TextDisabled("%s", config.noMatchesLabel.c_str());

        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}
} // namespace NLS::Editor::Inspector
