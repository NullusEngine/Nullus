#include "Assets/SpriteArtifact.h"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace NLS::Core::Assets
{
namespace
{
constexpr uint32_t Magic = 0x5250534e; // NSPR, little endian
constexpr size_t MaxPayloadBytes = 16 * 1024;
void WriteU32(std::vector<uint8_t>& out, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
}
void WriteString(std::vector<uint8_t>& out, const std::string& value)
{
    WriteU32(out, uint32_t(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
struct Reader
{
    std::span<const uint8_t> bytes;
    size_t cursor = 0;
    uint32_t U32()
    {
        if (bytes.size() - cursor < 4) throw std::runtime_error("Truncated sprite artifact");
        uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) value |= uint32_t(bytes[cursor++]) << shift;
        return value;
    }
    std::string String()
    {
        const auto size = U32();
        if (size > 4096 || size > bytes.size() - cursor) throw std::runtime_error("Invalid sprite string size");
        std::string value(reinterpret_cast<const char*>(bytes.data() + cursor), size);
        cursor += size;
        return value;
    }
};
float Setting(const AssetMeta& meta, const char* key, float fallback)
{
    const auto found = meta.settings.find(key);
    if (found == meta.settings.end()) return fallback;
    size_t count = 0;
    const auto value = std::stof(found->second, &count);
    if (count != found->second.size()) throw std::runtime_error(std::string("Invalid sprite setting: ") + key);
    return value;
}
uint32_t BorderSetting(const AssetMeta& meta,const char* key)
{
    const auto found=meta.settings.find(key);if(found==meta.settings.end())return 0;
    size_t count=0;const auto value=std::stoull(found->second,&count);
    if(count!=found->second.size()||value>UINT32_MAX)throw std::runtime_error(std::string("Invalid Sprite border: ")+key);
    return uint32_t(value);
}
}

bool SpriteArtifact::Validate(std::string& error) const
{
    error.clear();
    const auto identity = NLS::Guid::TryParse(id);
    if (!identity || !identity->IsValid() || !textureAssetId.IsValid() || textureSubAssetKey.empty())
        error = "Sprite requires a stable identity and source texture reference";
    else if (name.size() > 4096 || textureSubAssetKey.size() > 4096)
        error = "Sprite name or reference exceeds the format limit";
    else if (sourceWidth == 0 || sourceHeight == 0 || width == 0 || height == 0 ||
        x > sourceWidth || y > sourceHeight || width > sourceWidth - x || height > sourceHeight - y)
        error = "Sprite rectangle is outside the source image";
    else if(uint64_t(border[0])+border[2]>width||uint64_t(border[1])+border[3]>height)
        error = "Sprite borders must fit inside its rectangle";
    else if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit < 0.0001f ||
        !std::isfinite(pivot[0]) || !std::isfinite(pivot[1]) ||
        pivot[0] < 0 || pivot[0] > 1 || pivot[1] < 0 || pivot[1] > 1)
        error = "Sprite PPU must be positive and pivot must be normalized";
    return error.empty();
}

std::array<float, 4> SpriteArtifact::LocalRect() const
{
    const auto w = float(width) / pixelsPerUnit, h = float(height) / pixelsPerUnit;
    return {-pivot[0] * w, -pivot[1] * h, (1 - pivot[0]) * w, (1 - pivot[1]) * h};
}

std::array<float, 4> SpriteArtifact::UvRect() const
{
    return {float(x) / sourceWidth, float(y) / sourceHeight,
        float(x + width) / sourceWidth, float(y + height) / sourceHeight};
}

std::string SpriteArtifact::SubAssetKey() const { return "sprite:" + id; }

bool IsSpriteTexture(const AssetMeta& meta)
{
    const auto type = meta.settings.find("TEXTURE_TYPE");
    return meta.assetType == AssetType::Texture && type != meta.settings.end() && type->second == "sprite";
}

std::optional<SpriteArtifact> BuildSingleSpriteArtifact(const AssetMeta& meta, uint32_t sourceWidth,
    uint32_t sourceHeight, std::string textureSubAssetKey, std::string name, std::string& error)
{
    try
    {
        SpriteArtifact sprite;
        const auto identity = meta.settings.find("SPRITE_ID");
        sprite.id = identity == meta.settings.end()
            ? NLS::Guid::NewDeterministic(meta.id.ToString() + ":sprite:single").ToString() : identity->second;
        sprite.name = std::move(name);
        sprite.textureAssetId = meta.id;
        sprite.textureSubAssetKey = std::move(textureSubAssetKey);
        sprite.sourceWidth = sprite.width = sourceWidth;
        sprite.sourceHeight = sprite.height = sourceHeight;
        sprite.pixelsPerUnit = Setting(meta, "SPRITE_PIXELS_PER_UNIT", 100);
        sprite.pivot = {Setting(meta, "SPRITE_PIVOT_X", 0.5f), Setting(meta, "SPRITE_PIVOT_Y", 0.5f)};
        sprite.border={BorderSetting(meta,"SPRITE_BORDER_LEFT"),BorderSetting(meta,"SPRITE_BORDER_BOTTOM"),
            BorderSetting(meta,"SPRITE_BORDER_RIGHT"),BorderSetting(meta,"SPRITE_BORDER_TOP")};
        if (!sprite.Validate(error)) return {};
        return sprite;
    }
    catch (const std::exception& exception) { error = exception.what(); return {}; }
}

std::vector<uint8_t> SerializeSpriteArtifact(const SpriteArtifact& sprite)
{
    std::string error;
    if (!sprite.Validate(error)) return {};
    std::vector<uint8_t> out;
    const bool bordered=sprite.border!=std::array<uint32_t,4>{};
    WriteU32(out, Magic); WriteU32(out, bordered?SpriteArtifactPayloadVersion:1);
    WriteString(out, sprite.id); WriteString(out, sprite.name);
    WriteString(out, sprite.textureAssetId.ToString()); WriteString(out, sprite.textureSubAssetKey);
    for (auto value : {sprite.sourceWidth, sprite.sourceHeight, sprite.x, sprite.y, sprite.width, sprite.height}) WriteU32(out, value);
    for (auto value : {sprite.pivot[0], sprite.pivot[1], sprite.pixelsPerUnit}) WriteU32(out, std::bit_cast<uint32_t>(value));
    if(bordered)for(auto value:sprite.border)WriteU32(out,value);
    return out;
}

std::optional<SpriteArtifact> DeserializeSpriteArtifact(std::span<const uint8_t> bytes, std::string& error)
{
    error.clear();
    if (bytes.size() > MaxPayloadBytes) { error = "Sprite artifact exceeds size limit"; return {}; }
    try
    {
        Reader in {bytes};
        if(in.U32()!=Magic){error="Unsupported sprite artifact format";return {};}
        const auto version=in.U32();
        if(version!=1&&version!=SpriteArtifactPayloadVersion){error="Unsupported sprite artifact version";return {};}
        SpriteArtifact sprite;
        sprite.id = in.String(); sprite.name = in.String();
        const auto guid = NLS::Guid::TryParse(in.String());
        if (!guid) { error = "Invalid source texture GUID"; return {}; }
        sprite.textureAssetId = AssetId(*guid); sprite.textureSubAssetKey = in.String();
        sprite.sourceWidth = in.U32(); sprite.sourceHeight = in.U32();
        sprite.x = in.U32(); sprite.y = in.U32(); sprite.width = in.U32(); sprite.height = in.U32();
        sprite.pivot = {std::bit_cast<float>(in.U32()), std::bit_cast<float>(in.U32())};
        sprite.pixelsPerUnit = std::bit_cast<float>(in.U32());
        if(version>=2)for(auto& value:sprite.border)value=in.U32();
        if (in.cursor != bytes.size()) { error = "Unexpected trailing sprite data"; return {}; }
        if (!sprite.Validate(error)) return {};
        return sprite;
    }
    catch (const std::exception& exception) { error = exception.what(); return {}; }
}

bool AppendSingleSpriteArtifact(ArtifactWriteRequest& request, const AssetMeta& meta,
    uint32_t sourceWidth, uint32_t sourceHeight, const std::string& textureSubAssetKey,
    const std::string& name, std::string& error)
{
    if (!IsSpriteTexture(meta)) return true;
    const auto sprite = BuildSingleSpriteArtifact(meta, sourceWidth, sourceHeight, textureSubAssetKey, name, error);
    if (!sprite) return false;
    ArtifactPayload payload(sprite->SubAssetKey(), ArtifactType::Sprite, "sprite", sprite->name,
        "Sprites/" + sprite->id + ".nsprite", SerializeSpriteArtifact(*sprite));
    payload.dependencies.push_back({AssetDependencyKind::ImportedArtifact, meta.id.ToString(), textureSubAssetKey});
    request.artifacts.push_back(std::move(payload));
    return true;
}
}
