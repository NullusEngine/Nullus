#include "Rendering/Assets/MaterialConversion.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "Rendering/Resources/MaterialSerialization.h"

namespace NLS::Render::Assets
{
namespace
{
constexpr std::string_view kDefaultShaderLabMaterialShaderPath =
    "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader";

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

const ImportedSceneNamedRecord* FindTexture(
    const ImportedScene& scene,
    const std::string& textureKey)
{
    const auto found = std::find_if(
        scene.textures.begin(),
        scene.textures.end(),
        [&textureKey](const ImportedSceneNamedRecord& texture)
        {
            return texture.sourceKey == textureKey;
        });
    return found != scene.textures.end() ? &*found : nullptr;
}

std::vector<std::string> TextureIdentityTokens(const std::string_view identity)
{
    const auto stem = std::filesystem::path(std::string(identity)).stem().string();
    std::vector<std::string> tokens;
    std::string token;
    for (size_t index = 0u; index < stem.size(); ++index)
    {
        const auto character = static_cast<unsigned char>(stem[index]);
        const bool camelBoundary =
            index > 0u &&
            std::isupper(character) != 0 &&
            std::islower(static_cast<unsigned char>(stem[index - 1u])) != 0;
        if (std::isalnum(character) == 0 || camelBoundary)
        {
            if (!token.empty())
            {
                tokens.push_back(std::move(token));
                token.clear();
            }
            if (std::isalnum(character) == 0)
                continue;
        }
        token.push_back(static_cast<char>(std::tolower(character)));
    }
    if (!token.empty())
        tokens.push_back(std::move(token));
    return tokens;
}

bool IdentitySuggestsNormalMap(const std::string& identity)
{
    const auto tokens = TextureIdentityTokens(identity);
    return std::any_of(
        tokens.begin(),
        tokens.end(),
        [](const std::string& token)
        {
            return token == "normal" || token == "normalmap";
        });
}

bool TextureSuggestsNormalMap(const ImportedSceneNamedRecord& texture)
{
    return IdentitySuggestsNormalMap(texture.uri) ||
        IdentitySuggestsNormalMap(texture.name) ||
        IdentitySuggestsNormalMap(texture.sourceKey);
}

bool IsDataUri(const std::string& uri)
{
    return ToLower(uri).rfind("data:", 0u) == 0u;
}

std::string NormalizeResourcePath(std::filesystem::path path)
{
    return path.lexically_normal().generic_string();
}

std::string ResolveTextureResourcePath(
    const ImportedSceneNamedRecord& texture,
    const MaterialConversionContext& context)
{
    const auto importedArtifact = context.importedTextureArtifactPaths.find(texture.sourceKey);
    if (importedArtifact != context.importedTextureArtifactPaths.end() &&
        !importedArtifact->second.empty())
    {
        return NormalizeResourcePath(importedArtifact->second);
    }

    if (context.deferredTextureSourceKeys.find(texture.sourceKey) != context.deferredTextureSourceKeys.end())
        return {};

    if (texture.uri.empty() || IsDataUri(texture.uri))
        return {};

    const auto uriPath = std::filesystem::path(texture.uri);
    if (uriPath.is_absolute())
        return NormalizeResourcePath(uriPath);

    if (context.texturePathPrefix.empty())
        return NormalizeResourcePath(uriPath);

    return NormalizeResourcePath(context.texturePathPrefix / uriPath);
}

const ImportedSceneMaterialChannel* FindChannel(
    const ImportedSceneNamedRecord& material,
    const std::string& name)
{
    const auto found = std::find_if(
        material.materialChannels.begin(),
        material.materialChannels.end(),
        [&name](const ImportedSceneMaterialChannel& channel)
        {
            return ToLower(channel.name) == ToLower(name);
        });
    return found != material.materialChannels.end() ? &*found : nullptr;
}

const ImportedSceneMaterialChannel* FindBoundChannel(
    const ImportedSceneNamedRecord& material,
    const std::string& name)
{
    const auto loweredName = ToLower(name);
    const auto found = std::find_if(
        material.materialChannels.begin(),
        material.materialChannels.end(),
        [&loweredName](const ImportedSceneMaterialChannel& channel)
        {
            return !channel.textureKey.empty() && ToLower(channel.name) == loweredName;
        });
    return found != material.materialChannels.end() ? &*found : nullptr;
}

const ConvertedMaterialTextureSlot* FindTextureSlot(
    const ConvertedMaterialArtifact& material,
    const std::string& slot);

double FbxShininessToPbrRoughness(const double shininess)
{
    if (!std::isfinite(shininess))
        return 1.0;

    const auto clampedShininess = std::max(0.0, shininess);
    return std::clamp(std::sqrt(2.0 / (clampedShininess + 2.0)), 0.0, 1.0);
}

bool IsValidPbrUnitScalar(const double value)
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool IsIdentityColorFactor(const std::vector<double>& values)
{
    if (values.empty())
        return true;

    for (size_t index = 0u; index < std::min<size_t>(values.size(), 3u); ++index)
    {
        if (std::fabs(values[index] - 1.0) > 1e-6)
            return false;
    }
    return true;
}

bool IsNeutralRgbFactor(const std::vector<double>& values)
{
    if (values.size() < 3u)
        return false;

    const double red = values[0];
    const double green = values[1];
    const double blue = values[2];
    if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue))
        return false;

    return red > 0.0 &&
        red < 1.0 &&
        std::fabs(red - green) <= 1e-6 &&
        std::fabs(red - blue) <= 1e-6;
}

bool ShouldIgnoreFbxTexturedNeutralDiffuseTint(
    const ImportedSceneMaterialChannel& diffuse,
    const MaterialConversionContext& context)
{
    return context.ignoreFbxTexturedNeutralDiffuseTint && IsNeutralRgbFactor(diffuse.values);
}

bool MaterialIdentitySuggestsDecal(
    const std::string_view displayName,
    const std::string_view sourceSubAsset)
{
    const auto containsDecal = [](const std::string_view text)
    {
        constexpr std::string_view kDecal = "decal";
        std::string token;
        for (const char c : text)
        {
            if (std::isalpha(static_cast<unsigned char>(c)))
            {
                token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                continue;
            }

            if (token == kDecal)
                return true;
            token.clear();
        }

        if (token == kDecal)
            return true;

        for (size_t index = 0u; index + kDecal.size() <= text.size(); ++index)
        {
            bool matches = true;
            for (size_t offset = 0u; offset < kDecal.size(); ++offset)
            {
                if (static_cast<char>(std::tolower(static_cast<unsigned char>(text[index + offset]))) != kDecal[offset])
                {
                    matches = false;
                    break;
                }
            }
            if (!matches)
                continue;
            const bool startsCamelWord =
                std::isupper(static_cast<unsigned char>(text[index])) &&
                index > 0u &&
                std::isalpha(static_cast<unsigned char>(text[index - 1u])) &&
                (std::islower(static_cast<unsigned char>(text[index - 1u])) ||
                    (index + 1u < text.size() &&
                        std::isupper(static_cast<unsigned char>(text[index - 1u])) &&
                        std::islower(static_cast<unsigned char>(text[index + 1u]))));
            const bool startsToken = index == 0u ||
                !std::isalpha(static_cast<unsigned char>(text[index - 1u])) ||
                startsCamelWord;
            const auto after = index + kDecal.size();
            const bool endsToken = after == text.size() ||
                !std::isalpha(static_cast<unsigned char>(text[after])) ||
                std::isupper(static_cast<unsigned char>(text[after]));
            if (startsToken && endsToken)
                return true;
        }
        return false;
    };

    return containsDecal(displayName) || containsDecal(sourceSubAsset);
}

bool ShouldInferFbxBaseColorDecalAlpha(
    const ConvertedMaterialArtifact& material,
    const MaterialSourceModel sourceModel,
    const MaterialConversionContext& context)
{
    const auto* baseColor = FindTextureSlot(material, "BaseColor");
    const bool baseColorHasAlphaEvidence =
        baseColor != nullptr &&
        [&context, baseColor]()
        {
            const auto found = context.sourceTextureAlphaEvidence.find(baseColor->textureKey);
            return found != context.sourceTextureAlphaEvidence.end() && found->second;
        }();

    return sourceModel == MaterialSourceModel::FbxParserMaterial &&
        material.alphaMode == MaterialAlphaMode::Opaque &&
        baseColorHasAlphaEvidence &&
        FindTextureSlot(material, "Opacity") == nullptr &&
        MaterialIdentitySuggestsDecal(material.displayName, material.subAssetKey);
}

void AddDiagnostic(
    ConvertedMaterialArtifact& material,
    std::string code,
    std::string message)
{
    material.diagnostics.push_back({std::move(code), std::move(message)});
}

std::string WrapMode(const int value)
{
    switch (value)
    {
    case 33071: return "ClampToEdge";
    case 33648: return "MirroredRepeat";
    case 10497:
    default: return "Repeat";
    }
}

std::string FilterMode(const int value)
{
    switch (value)
    {
    case 9728: return "Nearest";
    case 9729: return "Linear";
    case 9984: return "NearestMipmapNearest";
    case 9985: return "LinearMipmapNearest";
    case 9986: return "NearestMipmapLinear";
    case 9987: return "LinearMipmapLinear";
    default: return "Linear";
    }
}

MaterialSamplerState ConvertSampler(const ImportedSceneTextureSampler& source)
{
    return {
        WrapMode(source.wrapS),
        WrapMode(source.wrapT),
        FilterMode(source.minFilter),
        FilterMode(source.magFilter)
    };
}

bool IsSupportedTextureEncoding(const ImportedSceneNamedRecord& texture)
{
    const auto mime = ToLower(texture.mimeType);
    if (mime.empty() || mime == "image/png" || mime == "image/jpeg" || mime == "image/jpg" || mime == "image/tga")
        return true;

    const auto uri = ToLower(texture.uri);
    return uri.ends_with(".png") ||
        uri.ends_with(".jpg") ||
        uri.ends_with(".jpeg") ||
        uri.ends_with(".tga");
}

void AddTextureSlot(
    const ImportedScene& scene,
    ConvertedMaterialArtifact& material,
    const MaterialConversionContext& context,
    std::string slot,
    std::string textureKey,
    const MaterialTextureColorSpace colorSpace,
    const ImportedSceneTextureSampler& sampler)
{
    if (textureKey.empty())
        return;

    const auto* texture = FindTexture(scene, textureKey);
    if (!texture)
    {
        AddDiagnostic(
            material,
            "material-missing-texture",
            "Material texture slot references a texture key that was not imported: " + textureKey);
        return;
    }

    if (!IsSupportedTextureEncoding(*texture))
    {
        AddDiagnostic(
            material,
            "material-unsupported-texture-encoding",
            "Material texture uses an unsupported encoding: " + texture->uri);
    }

    material.textureSlots.push_back({
        std::move(slot),
        textureKey,
        ResolveTextureResourcePath(*texture, context),
        colorSpace,
        ConvertSampler(sampler)
    });
}

void AddFactor(
    ConvertedMaterialArtifact& material,
    std::string name,
    std::vector<double> values)
{
    material.factors.push_back({std::move(name), std::move(values), false, 0.0});
}

void AddScalar(
    ConvertedMaterialArtifact& material,
    std::string name,
    const double value)
{
    material.factors.push_back({std::move(name), {}, true, value});
}

MaterialAlphaMode ConvertAlphaMode(const std::string& alphaMode)
{
    const auto lowered = ToLower(alphaMode);
    if (lowered == "blend")
        return MaterialAlphaMode::Blend;
    if (lowered == "mask")
        return MaterialAlphaMode::Mask;
    return MaterialAlphaMode::Opaque;
}

std::string AlphaModeName(const MaterialAlphaMode mode)
{
    switch (mode)
    {
    case MaterialAlphaMode::Mask: return "Mask";
    case MaterialAlphaMode::Blend: return "Blend";
    case MaterialAlphaMode::Opaque:
    default: return "Opaque";
    }
}

std::string SurfaceModeForAlphaMode(const MaterialAlphaMode mode)
{
    switch (mode)
    {
    case MaterialAlphaMode::Blend: return "Transparent";
    case MaterialAlphaMode::Mask:
    case MaterialAlphaMode::Opaque:
    default: return "Opaque";
    }
}

std::string SurfaceModeForMaterial(const ConvertedMaterialArtifact& material)
{
    if (material.alphaMode == MaterialAlphaMode::Blend &&
        MaterialIdentitySuggestsDecal(material.displayName, material.subAssetKey))
    {
        return "Decal";
    }

    return SurfaceModeForAlphaMode(material.alphaMode);
}

std::string ColorSpaceName(const MaterialTextureColorSpace colorSpace)
{
    return colorSpace == MaterialTextureColorSpace::SRgb ? "sRGB" : "Linear";
}

std::string FormatDouble(const double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6) << value;
    return stream.str();
}

std::string FormatVec4(const std::vector<double>& values)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6)
        << (values.size() > 0u ? values[0] : 1.0) << ' '
        << (values.size() > 1u ? values[1] : 1.0) << ' '
        << (values.size() > 2u ? values[2] : 1.0) << ' '
        << (values.size() > 3u ? values[3] : 1.0);
    return stream.str();
}

std::vector<double> ToVec4(std::vector<double> values, const std::vector<double>& fallback)
{
    if (values.empty())
        values = fallback;
    if (values.size() < 4u)
        values.resize(4u, 1.0);
    return values;
}

const ConvertedMaterialFactor* FindFactor(
    const ConvertedMaterialArtifact& material,
    const std::string& name)
{
    const auto found = std::find_if(
        material.factors.begin(),
        material.factors.end(),
        [&name](const ConvertedMaterialFactor& factor)
        {
            return factor.name == name;
        });
    return found != material.factors.end() ? &*found : nullptr;
}

double GetScalarFactor(
    const ConvertedMaterialArtifact& material,
    const std::string& name,
    const double fallback)
{
    const auto* factor = FindFactor(material, name);
    return factor && factor->hasScalar ? factor->scalar : fallback;
}

std::vector<double> GetVectorFactor(
    const ConvertedMaterialArtifact& material,
    const std::string& name,
    std::vector<double> fallback)
{
    const auto* factor = FindFactor(material, name);
    if (!factor || factor->hasScalar || factor->values.empty())
        return fallback;
    return factor->values;
}

const ConvertedMaterialTextureSlot* FindTextureSlot(
    const ConvertedMaterialArtifact& material,
    const std::string& slot)
{
    const auto found = std::find_if(
        material.textureSlots.begin(),
        material.textureSlots.end(),
        [&slot](const ConvertedMaterialTextureSlot& textureSlot)
        {
            return textureSlot.slot == slot;
        });
    return found != material.textureSlots.end() ? &*found : nullptr;
}

std::string TextureUniformPath(
    const ConvertedMaterialArtifact& material,
    const std::string& slot)
{
    const auto* textureSlot = FindTextureSlot(material, slot);
    if (!textureSlot)
        return {};
    return textureSlot->textureResourcePath;
}

std::string ResolveMaterialShaderResourcePath(const MaterialConversionContext& context)
{
    return context.shaderResourcePath.empty()
        ? std::string(kDefaultShaderLabMaterialShaderPath)
        : context.shaderResourcePath;
}

std::string SerializeMaterial(
    const ConvertedMaterialArtifact& material,
    const MaterialConversionContext& context)
{
    std::ostringstream stream;
    stream << "shaderLabMaterialVersion=1\n";
    stream << "shader=" << NLS::Render::Resources::EscapeMaterialField(ResolveMaterialShaderResourcePath(context)) << "\n";
    stream << "name=" << NLS::Render::Resources::EscapeMaterialField(material.displayName) << "\n";
    stream << "sourceSubAsset=" << NLS::Render::Resources::EscapeMaterialField(material.subAssetKey) << "\n";
    stream << "workflow=" << NLS::Render::Resources::EscapeMaterialField(material.workflow) << "\n";
    stream << "surfaceMode=" << SurfaceModeForMaterial(material) << "\n";
    stream << "alphaMode=" << AlphaModeName(material.alphaMode) << "\n";
    stream << "doubleSided=" << (material.doubleSided ? "true" : "false") << "\n";
    stream << "depthWrite=" << (material.alphaMode == MaterialAlphaMode::Blend ? "false" : "true") << "\n";
    stream << "renderQueueOverride=-1\n";

    auto baseColor = GetVectorFactor(material, "BaseColor", {1.0, 1.0, 1.0, 1.0});
    if (baseColor.size() < 4u)
        baseColor.resize(4u, 1.0);
    baseColor[3] = GetScalarFactor(material, "Alpha", baseColor[3]);

    stream << "property _BaseColor Color " << FormatVec4(baseColor) << "\n";
    stream << "property _Metallic Float " << FormatDouble(GetScalarFactor(material, "Metallic", 0.0)) << "\n";
    stream << "property _Roughness Float " << FormatDouble(GetScalarFactor(material, "Roughness", 1.0)) << "\n";
    stream << "property _AmbientOcclusion Float " << FormatDouble(GetScalarFactor(material, "OcclusionStrength", 1.0)) << "\n";
    const bool usesPackedMetallicRoughness = FindTextureSlot(material, "MetallicRoughness") != nullptr;
    stream << "property _MetallicMapChannel Vector "
        << (usesPackedMetallicRoughness
            ? "0.000000 0.000000 1.000000 0.000000"
            : "1.000000 0.000000 0.000000 0.000000")
        << "\n";
    stream << "property _RoughnessMapChannel Vector "
        << (usesPackedMetallicRoughness
            ? "0.000000 1.000000 0.000000 0.000000"
            : "1.000000 0.000000 0.000000 0.000000")
        << "\n";
    stream << "property _EmissiveColor Color "
        << FormatVec4(ToVec4(GetVectorFactor(material, "Emissive", {0.0, 0.0, 0.0, 1.0}), {0.0, 0.0, 0.0, 1.0})) << "\n";
    stream << "property _SpecularColor Color "
        << FormatVec4(ToVec4(GetVectorFactor(material, "Specular", {0.0, 0.0, 0.0, 1.0}), {0.0, 0.0, 0.0, 1.0})) << "\n";
    stream << "property _NormalScale Float " << FormatDouble(GetScalarFactor(material, "NormalScale", 1.0)) << "\n";
    stream << "property _Cutoff Float " << FormatDouble(material.alphaCutoff) << "\n";

    const auto writeTextureProperty = [&stream, &material](const std::string& name, const std::string& slot)
    {
        if (const auto path = TextureUniformPath(material, slot); !path.empty())
            stream << "property " << name << " Texture2D " << NLS::Render::Resources::EscapeMaterialField(path) << "\n";
    };
    const auto writeTexturePropertyWithFallback =
        [&stream, &material](const std::string& name, const std::string& preferredSlot, const std::string& fallbackSlot)
    {
        if (const auto path = TextureUniformPath(material, preferredSlot); !path.empty())
        {
            stream << "property " << name << " Texture2D " << NLS::Render::Resources::EscapeMaterialField(path) << "\n";
            return;
        }
        if (const auto path = TextureUniformPath(material, fallbackSlot); !path.empty())
            stream << "property " << name << " Texture2D " << NLS::Render::Resources::EscapeMaterialField(path) << "\n";
    };

    writeTextureProperty("_BaseMap", "BaseColor");
    writeTexturePropertyWithFallback("_MetallicMap", "Metallic", "MetallicRoughness");
    writeTexturePropertyWithFallback("_RoughnessMap", "Roughness", "MetallicRoughness");
    writeTextureProperty("_OcclusionMap", "Occlusion");
    writeTextureProperty("_NormalMap", "Normal");
    writeTextureProperty("_OpacityMap", "Opacity");
    writeTextureProperty("_EmissiveMap", "Emissive");
    writeTextureProperty("_SpecularMap", "Specular");

    if (material.alphaMode == MaterialAlphaMode::Mask)
        stream << "keyword _ALPHATEST_ON\n";
    if (FindTextureSlot(material, "Normal") != nullptr)
        stream << "keyword _NORMALMAP\n";

    auto writeTextureSlotOverride = [&stream](const ConvertedMaterialTextureSlot& slot, const std::string& shaderPropertyName)
    {
        stream << "textureSlot " << shaderPropertyName
            << " texture=" << NLS::Render::Resources::EscapeMaterialField(slot.textureKey)
            << " resourcePath=" << NLS::Render::Resources::EscapeMaterialField(slot.textureResourcePath)
            << " colorSpace=" << ColorSpaceName(slot.colorSpace)
            << " wrapS=" << slot.sampler.wrapS
            << " wrapT=" << slot.sampler.wrapT
            << " minFilter=" << slot.sampler.minFilter
            << " magFilter=" << slot.sampler.magFilter
            << "\n";
    };
    auto writeTextureSlotOverrideForSourceSlot = [&](const std::string& sourceSlot, const std::string& shaderPropertyName)
    {
        if (const auto* slot = FindTextureSlot(material, sourceSlot))
            writeTextureSlotOverride(*slot, shaderPropertyName);
    };

    writeTextureSlotOverrideForSourceSlot("BaseColor", "_BaseMap");
    writeTextureSlotOverrideForSourceSlot("Metallic", "_MetallicMap");
    writeTextureSlotOverrideForSourceSlot("Roughness", "_RoughnessMap");
    writeTextureSlotOverrideForSourceSlot("MetallicRoughness", "_MetallicMap");
    writeTextureSlotOverrideForSourceSlot("MetallicRoughness", "_RoughnessMap");
    writeTextureSlotOverrideForSourceSlot("Occlusion", "_OcclusionMap");
    writeTextureSlotOverrideForSourceSlot("Normal", "_NormalMap");
    writeTextureSlotOverrideForSourceSlot("Opacity", "_OpacityMap");
    writeTextureSlotOverrideForSourceSlot("Emissive", "_EmissiveMap");
    writeTextureSlotOverrideForSourceSlot("Specular", "_SpecularMap");

    for (const auto& slot : material.textureSlots)
    {
        const bool mappedToShaderProperty =
            slot.slot == "BaseColor" ||
            slot.slot == "Metallic" ||
            slot.slot == "Roughness" ||
            slot.slot == "MetallicRoughness" ||
            slot.slot == "Occlusion" ||
            slot.slot == "Normal" ||
            slot.slot == "Opacity" ||
            slot.slot == "Emissive" ||
            slot.slot == "Specular";
        if (!mappedToShaderProperty)
            writeTextureSlotOverride(slot, slot.slot);
    }
    for (const auto& factor : material.factors)
    {
        stream << "factor " << factor.name << " ";
        if (factor.hasScalar)
        {
            stream << FormatDouble(factor.scalar);
        }
        else
        {
            for (size_t index = 0u; index < factor.values.size(); ++index)
            {
                if (index > 0u)
                    stream << ' ';
                stream << FormatDouble(factor.values[index]);
            }
        }
        stream << "\n";
    }
    return stream.str();
}

std::string MakeMaterialArtifactHashName(const ConvertedMaterialArtifact& material)
{
    constexpr uint64_t offset = 1469598103934665603ull;
    constexpr uint64_t prime = 1099511628211ull;

    uint64_t hash = offset;
    const auto append = [&hash](const std::string_view value)
    {
        for (const auto character : value)
        {
            hash ^= static_cast<unsigned char>(character);
            hash *= prime;
        }
        hash ^= 0xffu;
        hash *= prime;
    };

    append(material.subAssetKey);
    append(material.displayName);
    append(material.serializedPayload);

    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

void ConvertGltf(
    const ImportedScene& scene,
    const ImportedSceneNamedRecord& source,
    const MaterialConversionContext& context,
    ConvertedMaterialArtifact& material)
{
    material.workflow = source.pbrWorkflow.empty() ? "metallic-roughness" : source.pbrWorkflow;
    material.doubleSided = source.doubleSided;
    material.alphaMode = ConvertAlphaMode(source.alphaMode);
    material.alphaCutoff = source.alphaCutoff;

    AddTextureSlot(scene, material, context, "BaseColor", source.baseColorTextureKey, MaterialTextureColorSpace::SRgb, source.sampler);
    AddTextureSlot(scene, material, context, "MetallicRoughness", source.metallicRoughnessTextureKey, MaterialTextureColorSpace::Linear, source.sampler);
    AddTextureSlot(scene, material, context, "Normal", source.normalTextureKey, MaterialTextureColorSpace::Linear, source.sampler);
    AddTextureSlot(scene, material, context, "Occlusion", source.occlusionTextureKey, MaterialTextureColorSpace::Linear, source.sampler);
    AddTextureSlot(scene, material, context, "Emissive", source.emissiveTextureKey, MaterialTextureColorSpace::SRgb, source.sampler);

    AddFactor(material, "BaseColor", source.baseColorFactor.empty() ? std::vector<double>{1.0, 1.0, 1.0, 1.0} : source.baseColorFactor);
    AddFactor(material, "Emissive", source.emissiveFactor.empty() ? std::vector<double>{0.0, 0.0, 0.0} : source.emissiveFactor);
    AddScalar(material, "Metallic", source.metallicFactor);
    AddScalar(material, "Roughness", source.roughnessFactor);
    AddScalar(material, "NormalScale", source.normalScale);
    AddScalar(material, "OcclusionStrength", source.occlusionStrength);
}

void ConvertParserChannels(
    const ImportedScene& scene,
    const ImportedSceneNamedRecord& source,
    ConvertedMaterialArtifact& material,
    const MaterialConversionContext& context,
    const MaterialSourceModel sourceModel)
{
    material.workflow = sourceModel == MaterialSourceModel::ObjMtl ? "mtl" : "parser-fbx";

    if (const auto* diffuse = FindChannel(source, "diffuse"))
    {
        AddTextureSlot(scene, material, context, "BaseColor", diffuse->textureKey, MaterialTextureColorSpace::SRgb, source.sampler);
        const bool hasBaseColorTexture = FindTextureSlot(material, "BaseColor") != nullptr;
        if (!diffuse->values.empty() &&
            sourceModel == MaterialSourceModel::FbxParserMaterial &&
            hasBaseColorTexture)
        {
            if (ShouldIgnoreFbxTexturedNeutralDiffuseTint(*diffuse, context))
            {
                AddDiagnostic(
                    material,
                    "material-ignored-fbx-textured-neutral-diffuse-tint",
                    "FBX neutral diffuse compatibility tint was ignored because a base-color texture is already bound.");
            }
            else if (!IsIdentityColorFactor(diffuse->values))
            {
                AddFactor(material, "BaseColor", diffuse->values);
            }
        }
        else if (!diffuse->values.empty())
        {
            AddFactor(material, "BaseColor", diffuse->values);
        }
    }
    const auto* normal = FindBoundChannel(source, "normal");
    if (normal)
        AddTextureSlot(scene, material, context, "Normal", normal->textureKey, MaterialTextureColorSpace::Linear, source.sampler);
    if (normal == nullptr)
    {
        for (const auto& bump : source.materialChannels)
        {
            if (ToLower(bump.name) != "bump" || bump.textureKey.empty())
                continue;

            const auto* bumpTexture = FindTexture(scene, bump.textureKey);
            if (bumpTexture && TextureSuggestsNormalMap(*bumpTexture))
            {
                AddTextureSlot(
                    scene,
                    material,
                    context,
                    "Normal",
                    bump.textureKey,
                    MaterialTextureColorSpace::Linear,
                    source.sampler);
                if (FindTextureSlot(material, "Normal") != nullptr)
                {
                    AddDiagnostic(
                        material,
                        "material-inferred-normal-map-from-bump-channel",
                        "Parser bump texture was promoted because its identity explicitly identifies a tangent-space normal map.");
                    break;
                }
            }
            else
            {
                AddDiagnostic(
                    material,
                    "material-ignored-bump-height-map",
                    "Parser bump/height texture was ignored because it is not identified as a tangent-space normal map.");
            }
        }
    }
    if (const auto* emissive = FindChannel(source, "emissive"))
    {
        AddTextureSlot(scene, material, context, "Emissive", emissive->textureKey, MaterialTextureColorSpace::SRgb, source.sampler);
        if (!emissive->values.empty())
            AddFactor(material, "Emissive", emissive->values);
    }
    if (const auto* occlusion = FindChannel(source, "occlusion"))
        AddTextureSlot(scene, material, context, "Occlusion", occlusion->textureKey, MaterialTextureColorSpace::Linear, source.sampler);
    if (const auto* metallic = FindChannel(source, "metallic"))
        AddTextureSlot(scene, material, context, "Metallic", metallic->textureKey, MaterialTextureColorSpace::Linear, source.sampler);
    const auto* roughnessChannel = FindChannel(source, "roughness");
    if (roughnessChannel)
        AddTextureSlot(scene, material, context, "Roughness", roughnessChannel->textureKey, MaterialTextureColorSpace::Linear, source.sampler);
    const bool hasValidRoughnessTexture = FindTextureSlot(material, "Roughness") != nullptr;
    if (const auto* opacity = FindChannel(source, "opacity"))
    {
        AddTextureSlot(scene, material, context, "Opacity", opacity->textureKey, MaterialTextureColorSpace::Linear, source.sampler);
        if (sourceModel == MaterialSourceModel::FbxParserMaterial &&
            FindTextureSlot(material, "Opacity") != nullptr)
        {
            material.alphaMode = MaterialAlphaMode::Blend;
        }
    }
    if (const auto* specular = FindChannel(source, "specular"))
    {
        AddTextureSlot(scene, material, context, "Specular", specular->textureKey, MaterialTextureColorSpace::SRgb, source.sampler);
        const bool ignoreFbxPbrCompatibilitySpecular =
            sourceModel == MaterialSourceModel::FbxParserMaterial &&
            FindTextureSlot(material, "Specular") == nullptr &&
            (FindTextureSlot(material, "Metallic") != nullptr || FindTextureSlot(material, "Roughness") != nullptr) &&
            IsIdentityColorFactor(specular->values);
        if (!specular->values.empty() && !ignoreFbxPbrCompatibilitySpecular)
            AddFactor(material, "Specular", specular->values);
    }
    const bool hasRoughnessScalar = roughnessChannel && roughnessChannel->hasScalar;
    const bool hasValidRoughnessScalar = hasRoughnessScalar && IsValidPbrUnitScalar(roughnessChannel->scalar);
    if (hasValidRoughnessScalar)
    {
        AddScalar(material, "Roughness", roughnessChannel->scalar);
    }
    else if (hasRoughnessScalar)
    {
        AddDiagnostic(
            material,
            "material-invalid-roughness-scalar",
            "Parser material roughness scalar is outside the PBR [0, 1] range and was ignored.");
    }
    if (const auto* metallic = FindChannel(source, "metallic"))
    {
        const bool hasMetallicTexture = FindTextureSlot(material, "Metallic") != nullptr;
        const bool useFbxTextureDefault =
            sourceModel == MaterialSourceModel::FbxParserMaterial &&
            hasMetallicTexture &&
            (!metallic->hasScalar || std::fabs(metallic->scalar) <= 1e-6);
        if (metallic->hasScalar || useFbxTextureDefault)
            AddScalar(material, "Metallic", useFbxTextureDefault ? 1.0 : metallic->scalar);
    }
    if (const auto* occlusion = FindChannel(source, "occlusion");
        occlusion && occlusion->hasScalar)
    {
        AddScalar(material, "OcclusionStrength", occlusion->scalar);
    }
    if (const auto* shininess = FindChannel(source, "shininess");
        shininess && shininess->hasScalar)
    {
        AddScalar(material, "SpecularPower", shininess->scalar);
        if (sourceModel == MaterialSourceModel::FbxParserMaterial && !hasValidRoughnessScalar && !hasValidRoughnessTexture)
            AddScalar(material, "Roughness", FbxShininessToPbrRoughness(shininess->scalar));
    }
    if (const auto* opacity = FindChannel(source, "opacity");
        opacity && opacity->hasScalar)
    {
        AddScalar(material, "Alpha", opacity->scalar);
        if (opacity->scalar < 1.0)
            material.alphaMode = MaterialAlphaMode::Blend;
    }
    if (const auto* doubleSided = FindChannel(source, "doubleSided");
        doubleSided && doubleSided->hasScalar)
    {
        material.doubleSided = doubleSided->scalar != 0.0;
    }
    if (const auto* illumination = FindChannel(source, "illumination");
        illumination && sourceModel == MaterialSourceModel::ObjMtl)
    {
        AddDiagnostic(
            material,
            "material-illumination-model-unsupported",
            "OBJ MTL illumination models are recorded as diagnostics when they cannot map directly to the runtime material model.");
    }
    if (ShouldInferFbxBaseColorDecalAlpha(material, sourceModel, context))
    {
        material.alphaMode = MaterialAlphaMode::Blend;
        AddDiagnostic(
            material,
            "material-inferred-fbx-decal-basecolor-alpha",
            "FBX decal material uses the base-color texture as its alpha-bearing decal texture.");
    }
}
}

ConvertedMaterialArtifact ConvertImportedSceneMaterial(
    const ImportedScene& scene,
    const ImportedSceneNamedRecord& material,
    const MaterialSourceModel sourceModel)
{
    return ConvertImportedSceneMaterial(scene, material, sourceModel, {});
}

ConvertedMaterialArtifact ConvertImportedSceneMaterial(
    const ImportedScene& scene,
    const ImportedSceneNamedRecord& material,
    const MaterialSourceModel sourceModel,
    const MaterialConversionContext& context)
{
    ConvertedMaterialArtifact result;
    result.subAssetKey = "material:" + (material.sourceKey.empty() ? material.name : material.sourceKey);
    result.displayName = material.name.empty() ? result.subAssetKey : material.name;

    switch (sourceModel)
    {
    case MaterialSourceModel::GltfPbrMetallicRoughness:
        ConvertGltf(scene, material, context, result);
        break;
    case MaterialSourceModel::FbxParserMaterial:
    case MaterialSourceModel::ObjMtl:
        ConvertParserChannels(scene, material, result, context, sourceModel);
        break;
    }

    result.serializedPayload = SerializeMaterial(result, context);
    return result;
}

std::vector<NLS::Core::Assets::ArtifactPayload> BuildMaterialArtifactPayloads(
    const std::vector<ConvertedMaterialArtifact>& materials)
{
    std::vector<NLS::Core::Assets::ArtifactPayload> payloads;
    payloads.reserve(materials.size());
    for (const auto& material : materials)
    {
        std::vector<uint8_t> bytes(material.serializedPayload.begin(), material.serializedPayload.end());
        payloads.push_back({
            material.subAssetKey,
            NLS::Core::Assets::ArtifactType::Material,
            "material",
            material.displayName,
            std::filesystem::path("materials") / MakeMaterialArtifactHashName(material),
            std::move(bytes)
        });
    }
    return payloads;
}
}
