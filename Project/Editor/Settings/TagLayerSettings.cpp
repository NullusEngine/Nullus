#include "Settings/TagLayerSettings.h"

#include <algorithm>

namespace NLS::Editor::Settings
{
namespace
{
const std::vector<std::string>& DefaultTags()
{
    static const std::vector<std::string> tags {
        "Untagged",
        "Respawn",
        "Finish",
        "EditorOnly",
        "MainCamera",
        "Player",
        "GameController"
    };
    return tags;
}

const std::array<std::string, TagLayerSettings::LayerCount>& DefaultLayers()
{
    static const std::array<std::string, TagLayerSettings::LayerCount> layers {
        "Default",
        "TransparentFX",
        "Ignore Raycast",
        "",
        "Water",
        "UI",
        "",
        "",
        "User Layer 8",
        "User Layer 9",
        "User Layer 10",
        "User Layer 11",
        "User Layer 12",
        "User Layer 13",
        "User Layer 14",
        "User Layer 15",
        "User Layer 16",
        "User Layer 17",
        "User Layer 18",
        "User Layer 19",
        "User Layer 20",
        "User Layer 21",
        "User Layer 22",
        "User Layer 23",
        "User Layer 24",
        "User Layer 25",
        "User Layer 26",
        "User Layer 27",
        "User Layer 28",
        "User Layer 29",
        "User Layer 30",
        "User Layer 31"
    };
    return layers;
}
} // namespace

const std::vector<std::string>& TagLayerSettings::GetTags()
{
    return DefaultTags();
}

const std::array<std::string, TagLayerSettings::LayerCount>& TagLayerSettings::GetLayers()
{
    return DefaultLayers();
}

std::map<int, std::string> TagLayerSettings::BuildTagChoices()
{
    std::map<int, std::string> choices;
    const auto& tags = GetTags();
    for (size_t index = 0; index < tags.size(); ++index)
        choices.emplace(static_cast<int>(index), tags[index]);
    return choices;
}

std::map<int, std::string> TagLayerSettings::BuildLayerChoices()
{
    std::map<int, std::string> choices;
    const auto& layers = GetLayers();
    for (size_t index = 0; index < layers.size(); ++index)
    {
        const auto name = layers[index].empty()
            ? "Layer " + std::to_string(index)
            : layers[index];
        choices.emplace(static_cast<int>(index), std::to_string(index) + ": " + name);
    }
    return choices;
}

int TagLayerSettings::FindTagIndex(const std::string& p_tag)
{
    const auto& tags = GetTags();
    const auto found = std::find(tags.begin(), tags.end(), p_tag);
    return found != tags.end() ? static_cast<int>(std::distance(tags.begin(), found)) : 0;
}

std::string TagLayerSettings::GetTagAt(int p_index)
{
    const auto& tags = GetTags();
    if (p_index < 0 || static_cast<size_t>(p_index) >= tags.size())
        return tags.front();
    return tags[static_cast<size_t>(p_index)];
}

std::string TagLayerSettings::GetLayerName(int p_layer)
{
    const auto& layers = GetLayers();
    if (p_layer < 0 || static_cast<size_t>(p_layer) >= layers.size())
        return {};
    return layers[static_cast<size_t>(p_layer)];
}
} // namespace NLS::Editor::Settings
