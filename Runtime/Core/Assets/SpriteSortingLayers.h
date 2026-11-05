#pragma once
#include "CoreDef.h"
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Core::Assets
{
struct SpriteSortingLayer { std::string id, name; };
struct NLS_CORE_API SpriteSortingLayers
{
    static constexpr const char* DefaultId = "00000000-0000-0000-0000-000000000001";
    static constexpr const char* RelativePath = "Settings/SpriteSortingLayers.json";
    std::vector<SpriteSortingLayer> layers {{DefaultId,"Default"}};
    int ResolveRank(std::string_view id) const;
    std::string Serialize() const;
    static bool Parse(std::string_view text, SpriteSortingLayers& value, std::string& error);
};
}
