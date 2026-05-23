#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

namespace NLS::Editor::Settings
{
class TagLayerSettings
{
public:
    static constexpr size_t LayerCount = 32;

    static const std::vector<std::string>& GetTags();
    static const std::array<std::string, LayerCount>& GetLayers();

    static std::map<int, std::string> BuildTagChoices();
    static std::map<int, std::string> BuildLayerChoices();
    static int FindTagIndex(const std::string& p_tag);
    static std::string GetTagAt(int p_index);
    static std::string GetLayerName(int p_layer);
};
} // namespace NLS::Editor::Settings
