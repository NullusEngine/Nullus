#include "Assets/SpriteSortingLayers.h"
#include <Guid.h>
#include <Json/json.hpp>
#include <algorithm>
#include <set>

namespace NLS::Core::Assets
{
int SpriteSortingLayers::ResolveRank(std::string_view id) const
{
    for (size_t i=0;i<layers.size();++i) if (layers[i].id == id) return int(i);
    if (id == "Default") return ResolveRank(DefaultId);
    const auto found = std::find_if(layers.begin(),layers.end(),[](const auto& layer){return layer.id==DefaultId;});
    return found == layers.end() ? 0 : int(found-layers.begin());
}
std::string SpriteSortingLayers::Serialize() const
{
    nlohmann::json array=nlohmann::json::array();
    for(const auto& layer:layers) array.push_back({{"id",layer.id},{"name",layer.name}});
    return nlohmann::json{{"version",1},{"layers",array}}.dump(2)+"\n";
}
bool SpriteSortingLayers::Parse(std::string_view text, SpriteSortingLayers& value, std::string& error)
{
    if(text.size()>128*1024) {error="Sorting layer settings exceed 128 KiB";return false;}
    const auto json=nlohmann::json::parse(text.begin(),text.end(),nullptr,false);
    if(!json.is_object()||!json.contains("version")||json["version"]!=1||!json.contains("layers")||!json["layers"].is_array()||json["layers"].empty()||json["layers"].size()>256)
    {error="Invalid sorting layer settings or unsupported version";return false;}
    SpriteSortingLayers parsed;parsed.layers.clear();std::set<std::string> ids,names;
    for(const auto& item:json["layers"])
    {
        if(!item.is_object()||!item.contains("id")||!item["id"].is_string()||!item.contains("name")||!item["name"].is_string())
        {error="Sorting layer requires a persistent ID and name";return false;}
        SpriteSortingLayer layer{item["id"].get<std::string>(),item["name"].get<std::string>()};
        const auto guid=Guid::TryParse(layer.id);
        if(!guid||!guid->IsValid()||layer.name.empty()||layer.name.size()>256||layer.name.find_first_not_of(" \t\r\n")==std::string::npos||!ids.insert(layer.id).second||!names.insert(layer.name).second)
        {error="Sorting layer IDs and names must be valid and unique";return false;}
        parsed.layers.push_back(std::move(layer));
    }
    if(!ids.contains(DefaultId)){error="The Default sorting layer cannot be removed";return false;}
    value=std::move(parsed);error.clear();return true;
}
}
