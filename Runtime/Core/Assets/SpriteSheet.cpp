#include "Assets/SpriteSheet.h"

#include <Json/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <unordered_set>

namespace NLS::Core::Assets
{
namespace
{
constexpr size_t MaxMetadataBytes = 16 * 1024 * 1024;
using Rect = std::array<uint32_t,4>;

bool ValidateSlices(std::span<const SpriteSliceDefinition> slices, std::string& error)
{
    if (slices.size() > MaxSpriteSlices) { error = "Sprite sheet exceeds the slice count limit"; return false; }
    std::unordered_set<std::string> identities;
    for (size_t index = 0; index < slices.size(); ++index)
    {
        const auto& slice = slices[index];
        const auto guid = Guid::TryParse(slice.id);
        if (!guid || !guid->IsValid() || !identities.insert(guid->ToString()).second)
            error = "Sprite slice requires a valid, unique persistent UUID";
        else if (slice.name.size() > 4096 || slice.name.find('\0') != std::string::npos)
            error = "Sprite slice name exceeds its format limit or contains a null character";
        else if (!slice.width || !slice.height || uint64_t(slice.x)+slice.width > UINT32_MAX || uint64_t(slice.y)+slice.height > UINT32_MAX)
            error = "Sprite slice rectangle is empty or exceeds the coordinate range";
        else if(uint64_t(slice.border[0])+slice.border[2]>slice.width||uint64_t(slice.border[1])+slice.border[3]>slice.height)
            error="Sprite borders must fit inside the slice rectangle";
        else if (!std::isfinite(slice.pixelsPerUnit) || slice.pixelsPerUnit < 0.0001f ||
            !std::isfinite(slice.pivot[0]) || !std::isfinite(slice.pivot[1]) ||
            slice.pivot[0] < 0 || slice.pivot[0] > 1 || slice.pivot[1] < 0 || slice.pivot[1] > 1)
            error = "Sprite slice requires positive PPU and a normalized pivot";
        else continue;
        error = "Slice " + std::to_string(index) + ": " + error;
        return false;
    }
    error.clear(); return true;
}

bool ReadU32(const nlohmann::json& object, const char* key, uint32_t& value)
{
    if (!object.contains(key) || !object[key].is_number()) return false;
    const auto number = object[key].get<double>();
    if (!std::isfinite(number) || number < 0 || number > UINT32_MAX || std::floor(number) != number) return false;
    value = uint32_t(number); return true;
}
}

bool HasCurrentSpriteCompilerDependency(const ArtifactManifest& manifest,const AssetMeta& meta)
{
    return !IsSpriteTexture(meta)||std::any_of(manifest.dependencies.begin(),manifest.dependencies.end(),[](const auto& dependency)
    {
        return dependency.kind==AssetDependencyKind::PostprocessorVersion&&dependency.value==SpriteCompilerDependency&&
            dependency.hashOrVersion==SpriteCompilerVersion;
    });
}

std::string SerializeSpriteSlices(std::span<const SpriteSliceDefinition> slices, std::string& error)
{
    if (!ValidateSlices(slices,error)) return {};
    const bool bordered=std::any_of(slices.begin(),slices.end(),[](const auto& slice){return slice.border!=std::array<uint32_t,4>{};});
    auto values = nlohmann::json::array();
    for (const auto& slice : slices)
    {
        values.push_back({{"id",Guid::TryParse(slice.id)->ToString()},{"name",slice.name},
            {"x",slice.x},{"y",slice.y},{"width",slice.width},{"height",slice.height},
            {"pivot",slice.pivot},{"pixelsPerUnit",slice.pixelsPerUnit}});
        if(bordered)values.back()["border"]=slice.border;
    }
    try
    {
        auto text = nlohmann::json{{"version",bordered?SpriteSliceMetadataVersion:1},{"sprites",std::move(values)}}.dump();
        if (text.size() > MaxMetadataBytes) { error = "Sprite slice metadata exceeds 16 MiB"; return {}; }
        error.clear(); return text;
    }
    catch (const std::exception& exception) { error = exception.what(); return {}; }
}

std::optional<std::vector<SpriteSliceDefinition>> DeserializeSpriteSlices(std::string_view text, std::string& error)
{
    if (text.size() > MaxMetadataBytes) { error = "Sprite slice metadata exceeds 16 MiB"; return {}; }
    const auto json = nlohmann::json::parse(text.begin(),text.end(),nullptr,false);
    uint32_t version = 0;
    if (!json.is_object() || !ReadU32(json,"version",version) || (version!=1&&version!=SpriteSliceMetadataVersion) ||
        !json.contains("sprites") || !json["sprites"].is_array() || json["sprites"].size() > MaxSpriteSlices)
    { error = "Invalid Sprite slice metadata or unsupported version"; return {}; }
    std::vector<SpriteSliceDefinition> result;
    result.reserve(json["sprites"].size());
    for (const auto& item : json["sprites"])
    {
        SpriteSliceDefinition slice;
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
            !item.contains("name") || !item["name"].is_string() ||
            !ReadU32(item,"x",slice.x) || !ReadU32(item,"y",slice.y) ||
            !ReadU32(item,"width",slice.width) || !ReadU32(item,"height",slice.height) ||
            !item.contains("pivot") || !item["pivot"].is_array() || item["pivot"].size() != 2 ||
            !item["pivot"][0].is_number() || !item["pivot"][1].is_number() ||
            !item.contains("pixelsPerUnit") || !item["pixelsPerUnit"].is_number())
        { error = "Sprite slice requires ID, name, integer rectangle, pivot and PPU"; return {}; }
        slice.id = item["id"].get<std::string>(); slice.name = item["name"].get<std::string>();
        slice.pivot = {item["pivot"][0].get<float>(),item["pivot"][1].get<float>()};
        slice.pixelsPerUnit = item["pixelsPerUnit"].get<float>();
        if(version>=2)
        {
            if(!item.contains("border")||!item["border"].is_array()||item["border"].size()!=4)
            {error="Sprite v2 slices require Left/Bottom/Right/Top pixel borders";return {};}
            for(size_t i=0;i<4;++i)
            {
                const auto& value=item["border"][i];
                if(!value.is_number_unsigned()||value.get<uint64_t>()>UINT32_MAX)
                {error="Sprite borders must be nonnegative integer pixels";return {};}
                slice.border[i]=value.get<uint32_t>();
            }
        }
        result.push_back(std::move(slice));
    }
    if (!ValidateSlices(result,error)) return {};
    for (auto& slice : result) slice.id = Guid::TryParse(slice.id)->ToString();
    return result;
}

std::optional<std::vector<SpriteSliceDefinition>> GenerateGridSpriteSlices(
    uint32_t sourceWidth, uint32_t sourceHeight, const SpriteGridSettings& settings,
    std::span<const SpriteSliceDefinition> existing, std::string& error)
{
    if (!sourceWidth || !sourceHeight || !settings.cellWidth || !settings.cellHeight ||
        settings.offsetX >= sourceWidth || settings.offsetY >= sourceHeight)
    { error = "Grid requires a nonempty image, positive cell size and an offset inside the image"; return {}; }
    if (settings.namePrefix.size() > 4000) { error = "Grid name prefix exceeds its format limit"; return {}; }
    if (!ValidateSlices(existing,error)) return {};
    std::map<Rect,const SpriteSliceDefinition*> previous;
    for (const auto& slice : existing)
        if (!previous.emplace(Rect{slice.x,slice.y,slice.width,slice.height},&slice).second)
        { error = "Grid identity matching is ambiguous because existing rectangles overlap exactly"; return {}; }
    const uint64_t strideX = uint64_t(settings.cellWidth)+settings.paddingX;
    const uint64_t strideY = uint64_t(settings.cellHeight)+settings.paddingY;
    const auto cells = [&](uint64_t extent,uint64_t cell,uint64_t stride)
    { return settings.includePartialCells ? (extent-1)/stride+1 : extent < cell ? uint64_t(0) : (extent-cell)/stride+1; };
    const auto columns = cells(sourceWidth-settings.offsetX,settings.cellWidth,strideX);
    const auto rows = cells(sourceHeight-settings.offsetY,settings.cellHeight,strideY);
    if (columns*rows > MaxSpriteSlices) { error = "Grid exceeds the Sprite slice count limit"; return {}; }
    std::vector<SpriteSliceDefinition> result;
    result.reserve(size_t(columns*rows));
    for (uint64_t row = 0; row < rows; ++row)
        for (uint64_t column = 0; column < columns; ++column)
        {
            SpriteSliceDefinition slice;
            slice.x = uint32_t(settings.offsetX+column*strideX); slice.y = uint32_t(settings.offsetY+row*strideY);
            slice.width = std::min(settings.cellWidth,sourceWidth-slice.x);
            slice.height = std::min(settings.cellHeight,sourceHeight-slice.y);
            const auto found = previous.find({slice.x,slice.y,slice.width,slice.height});
            if (found != previous.end()) slice = *found->second;
            else
            {
                slice.id = Guid::New().ToString();
                slice.name = settings.namePrefix + "_" + std::to_string(row) + "_" + std::to_string(column);
                slice.pivot = settings.pivot; slice.pixelsPerUnit = settings.pixelsPerUnit;
            }
            result.push_back(std::move(slice));
        }
    if (!ValidateSlices(result,error)) return {};
    return result;
}

std::optional<std::vector<SpriteSliceDefinition>> GenerateAlphaSpriteSlices(
    std::span<const uint8_t> rgba,uint32_t width,uint32_t height,uint8_t alphaThreshold,
    uint32_t minimumOpaquePixels,const SpriteGridSettings& defaults,
    std::span<const SpriteSliceDefinition> existing,std::string& error)
{
    const uint64_t pixels = uint64_t(width)*height;
    if (!pixels || pixels > 64ull*1024*1024 || rgba.size() != pixels*4)
    { error = "Automatic slicing requires a complete RGBA image of at most 64 million pixels"; return {}; }
    if (!ValidateSlices(existing,error)) return {};
    if (defaults.namePrefix.size() > 4000 || !std::isfinite(defaults.pixelsPerUnit) || defaults.pixelsPerUnit < 0.0001f)
    { error = "Automatic slicing defaults have an invalid name or PPU"; return {}; }
    std::map<Rect,const SpriteSliceDefinition*> previous;
    for (const auto& slice : existing)
        if (!previous.emplace(Rect{slice.x,slice.y,slice.width,slice.height},&slice).second)
        { error = "Automatic slice identity matching is ambiguous for duplicate rectangles"; return {}; }
    std::vector<uint8_t> visited(size_t((pixels+7)/8));
    const auto mark = [&](uint32_t index)
    {
        auto& byte = visited[index/8]; const uint8_t bit = uint8_t(1u << (index%8));
        const bool already = (byte&bit) != 0; byte |= bit; return already;
    };
    std::vector<uint32_t> queue;
    std::vector<SpriteSliceDefinition> result;
    std::unordered_set<std::string> reused;
    for (uint32_t seed = 0; seed < pixels; ++seed)
    {
        if (mark(seed) || rgba[size_t(seed)*4+3] <= alphaThreshold) continue;
        queue.clear(); queue.push_back(seed);
        uint32_t left = seed%width, right = left, top = seed/width, bottom = top;
        for (size_t cursor = 0; cursor < queue.size(); ++cursor)
        {
            const uint32_t index = queue[cursor], x = index%width, y = index/width;
            left = std::min(left,x); right = std::max(right,x); top = std::min(top,y); bottom = std::max(bottom,y);
            const auto visit = [&](uint32_t next)
            {
                if (!mark(next) && rgba[size_t(next)*4+3] > alphaThreshold) queue.push_back(next);
            };
            if (x) visit(index-1);
            if (x+1 < width) visit(index+1);
            if (y) visit(index-width);
            if (y+1 < height) visit(index+width);
        }
        if (queue.size() < std::max(1u,minimumOpaquePixels)) continue;
        if (result.size() == MaxSpriteSlices) { error = "Automatic slicing exceeds the Sprite slice count limit"; return {}; }
        SpriteSliceDefinition slice;
        slice.x = left; slice.y = top; slice.width = right-left+1; slice.height = bottom-top+1;
        const auto found = previous.find({slice.x,slice.y,slice.width,slice.height});
        if (found != previous.end() && reused.insert(found->second->id).second) slice = *found->second;
        else
        {
            slice.id = Guid::New().ToString(); slice.name = defaults.namePrefix+"_"+std::to_string(result.size());
            slice.pivot = defaults.pivot; slice.pixelsPerUnit = defaults.pixelsPerUnit;
        }
        result.push_back(std::move(slice));
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b)
    { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    if (!ValidateSlices(result,error)) return {};
    return result;
}

std::optional<std::vector<SpriteArtifact>> BuildSpriteArtifacts(const AssetMeta& meta,
    uint32_t sourceWidth,uint32_t sourceHeight,const std::string& textureSubAssetKey,
    const std::string& name,std::string& error)
{
    error.clear();
    if (!IsSpriteTexture(meta)) return std::vector<SpriteArtifact>{};
    const auto foundMode = meta.settings.find(SpriteImportModeSetting);
    auto mode = foundMode == meta.settings.end() ? std::string("single") : foundMode->second;
    std::transform(mode.begin(),mode.end(),mode.begin(),[](unsigned char c){return char(std::tolower(c));});
    if (mode == "single")
    {
        const auto sprite = BuildSingleSpriteArtifact(meta,sourceWidth,sourceHeight,textureSubAssetKey,name,error);
        if (!sprite) return {};
        return std::vector<SpriteArtifact>{*sprite};
    }
    if (mode != "multiple") { error = "Sprite import mode must be Single or Multiple"; return {}; }
    if (!meta.id.IsValid() || textureSubAssetKey.empty() || !sourceWidth || !sourceHeight)
    { error = "Sprite sheet requires a source texture identity and nonempty dimensions"; return {}; }
    const auto found = meta.settings.find(SpriteSlicesSetting);
    if (found == meta.settings.end()) return std::vector<SpriteArtifact>{};
    const auto slices = DeserializeSpriteSlices(found->second,error);
    if (!slices) return {};
    std::vector<SpriteArtifact> sprites; sprites.reserve(slices->size());
    for (const auto& slice : *slices)
    {
        SpriteArtifact sprite;
        sprite.id = slice.id; sprite.name = slice.name;
        sprite.textureAssetId = meta.id; sprite.textureSubAssetKey = textureSubAssetKey;
        sprite.sourceWidth = sourceWidth; sprite.sourceHeight = sourceHeight;
        sprite.x = slice.x; sprite.y = slice.y; sprite.width = slice.width; sprite.height = slice.height;
        sprite.pivot = slice.pivot; sprite.pixelsPerUnit = slice.pixelsPerUnit;
        sprite.border=slice.border;
        if (!sprite.Validate(error)) { error = "Sprite " + slice.name + ": " + error; return {}; }
        sprites.push_back(std::move(sprite));
    }
    return sprites;
}

bool AppendSpriteArtifacts(ArtifactWriteRequest& request,const AssetMeta& meta,
    uint32_t sourceWidth,uint32_t sourceHeight,const std::string& textureSubAssetKey,
    const std::string& name,std::string& error)
{
    const auto sprites = BuildSpriteArtifacts(meta,sourceWidth,sourceHeight,textureSubAssetKey,name,error);
    if (!sprites) return false;
    std::unordered_set<std::string> keys;
    for (const auto& artifact : request.artifacts) keys.insert(artifact.subAssetKey);
    std::vector<ArtifactPayload> pending; pending.reserve(sprites->size());
    for (const auto& sprite : *sprites)
    {
        if (!keys.insert(sprite.SubAssetKey()).second) { error = "Duplicate Sprite artifact key in import request"; return false; }
        ArtifactPayload payload(sprite.SubAssetKey(),ArtifactType::Sprite,"sprite",sprite.name,
            "Sprites/"+sprite.id+".nsprite",SerializeSpriteArtifact(sprite));
        if (payload.payload.empty()) { error = "Could not serialize validated Sprite artifact"; return false; }
        payload.dependencies.push_back({AssetDependencyKind::ImportedArtifact,meta.id.ToString(),textureSubAssetKey});
        pending.push_back(std::move(payload));
    }
    request.artifacts.insert(request.artifacts.end(),std::make_move_iterator(pending.begin()),std::make_move_iterator(pending.end()));
    if(IsSpriteTexture(meta))request.dependencies.push_back({AssetDependencyKind::PostprocessorVersion,SpriteCompilerDependency,SpriteCompilerVersion});
    error.clear(); return true;
}
}
