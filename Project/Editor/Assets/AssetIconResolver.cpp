#include "Assets/AssetIconResolver.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "Assets/AssetMeta.h"

namespace NLS::Editor::Assets
{
namespace
{
    using Icons::IconId;

    constexpr IconId kDefaultIcon { "editor.icon.asset.default" };
    constexpr IconId kFolderIcon { "editor.icon.asset.folder" };
    constexpr IconId kTextureIcon { "editor.icon.asset.texture" };
    constexpr IconId kMaterialIcon { "editor.icon.asset.material" };
    constexpr IconId kMeshIcon { "editor.icon.asset.mesh" };
    constexpr IconId kPrefabIcon { "editor.icon.asset.prefab" };
    constexpr IconId kSceneIcon { "editor.icon.asset.scene" };
    constexpr IconId kShaderIcon { "editor.icon.asset.shader" };
    constexpr IconId kScriptIcon { "editor.icon.asset.script" };
    constexpr IconId kCSharpIcon { "editor.icon.asset.script.csharp" };
    constexpr IconId kLuaIcon { "editor.icon.asset.script.lua" };
    constexpr IconId kFontIcon { "editor.icon.asset.font" };
    constexpr IconId kAudioIcon { "editor.icon.asset.audio" };
    constexpr IconId kUIDocumentIcon { "editor.icon.asset.ui-document" };
    constexpr IconId kUIStyleIcon { "editor.icon.asset.ui-style" };
    constexpr IconId kLocalizationIcon { "editor.icon.asset.localization" };
    constexpr IconId kVfxSystemIcon { "editor.icon.asset.vfx-system" };
    constexpr IconId kVfxScriptIcon { "editor.icon.asset.vfx-script" };
    constexpr IconId kVfxEmitterIcon { "editor.icon.asset.vfx-emitter" };
    constexpr IconId kShaderGraphIcon { "editor.icon.asset.shader-graph" };
    constexpr IconId kShaderFunctionIcon { "editor.icon.asset.shader-function" };

    std::string ToLower(std::string p_text)
    {
        std::transform(
            p_text.begin(),
            p_text.end(),
            p_text.begin(),
            [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return p_text;
    }
}

AssetIconResolver& AssetIconResolver::Default()
{
    static AssetIconResolver resolver;
    return resolver;
}

Icons::IconId AssetIconResolver::ResolveAssetType(
    const NLS::Core::Assets::AssetType p_type) const
{
    using NLS::Core::Assets::AssetType;
    switch (p_type)
    {
    case AssetType::ModelScene:      return kMeshIcon;
    case AssetType::Texture:         return kTextureIcon;
    case AssetType::Shader:          return kShaderIcon;
    case AssetType::ShaderGraph:     return kShaderGraphIcon;
    case AssetType::ShaderFunction:  return kShaderFunctionIcon;
    case AssetType::Material:        return kMaterialIcon;
    case AssetType::Audio:           return kAudioIcon;
    case AssetType::Scene:           return kSceneIcon;
    case AssetType::Script:          return kScriptIcon;
    case AssetType::Prefab:          return kPrefabIcon;
    case AssetType::Font:            return kFontIcon;
    case AssetType::UIDocument:      return kUIDocumentIcon;
    case AssetType::UIStyleSheet:    return kUIStyleIcon;
    case AssetType::LocalizationTable: return kLocalizationIcon;
    case AssetType::VfxSystem:       return kVfxSystemIcon;
    case AssetType::VfxScript:       return kVfxScriptIcon;
    case AssetType::VfxEmitter:      return kVfxEmitterIcon;

    case AssetType::Unknown:
    case AssetType::Count:
        break;
    }
    return kDefaultIcon;
}

Icons::IconId AssetIconResolver::ResolvePath(const std::string_view p_path) const
{
    const std::filesystem::path path{ std::string(p_path) };
    const std::string extension = NormalizeExtension(path.extension().string());

    if (const auto plugin = m_extensionIcons.find(extension); plugin != m_extensionIcons.end())
        return IconId{ plugin->second };

    if (extension == ".cs")
        return kCSharpIcon;
    if (extension == ".lua")
        return kLuaIcon;

    return ResolveAssetType(NLS::Core::Assets::InferAssetType(path));
}

Icons::IconId AssetIconResolver::ResolveImporter(const std::string_view p_importerId) const
{
    if (p_importerId.empty())
        return kDefaultIcon;

    const auto found = m_importerIcons.find(NormalizeKey(p_importerId));
    return found != m_importerIcons.end() ? IconId{ found->second } : kDefaultIcon;
}

Icons::IconId AssetIconResolver::ResolveBrowserType(const AssetBrowserItemType p_type) const
{
    using NLS::Core::Assets::AssetType;
    switch (p_type)
    {
    case AssetBrowserItemType::Folder:  return kFolderIcon;
    case AssetBrowserItemType::Model:
    case AssetBrowserItemType::Mesh:    return ResolveAssetType(AssetType::ModelScene);
    case AssetBrowserItemType::Prefab:  return ResolveAssetType(AssetType::Prefab);
    case AssetBrowserItemType::Material: return ResolveAssetType(AssetType::Material);
    case AssetBrowserItemType::Texture: return ResolveAssetType(AssetType::Texture);
    case AssetBrowserItemType::Shader:  return ResolveAssetType(AssetType::Shader);
    case AssetBrowserItemType::Scene:   return ResolveAssetType(AssetType::Scene);
    case AssetBrowserItemType::Script:  return ResolveAssetType(AssetType::Script);
    case AssetBrowserItemType::All:
    case AssetBrowserItemType::Other:
    case AssetBrowserItemType::Count:
        break;
    }
    return kDefaultIcon;
}

Icons::IconId AssetIconResolver::ResolveThumbnailFallback(
    const AssetThumbnailKind p_kind,
    const std::string_view p_sourcePath) const
{
    using NLS::Core::Assets::AssetType;
    switch (p_kind)
    {
    case AssetThumbnailKind::Texture:        return ResolveAssetType(AssetType::Texture);
    case AssetThumbnailKind::MaterialSphere: return ResolveAssetType(AssetType::Material);
    case AssetThumbnailKind::ModelPreview:   return ResolveAssetType(AssetType::ModelScene);
    case AssetThumbnailKind::PrefabPreview:  return ResolveAssetType(AssetType::Prefab);
    case AssetThumbnailKind::Icon:
        if (!p_sourcePath.empty())
            return ResolvePath(p_sourcePath);
        break;
    case AssetThumbnailKind::GenericPreview:
    case AssetThumbnailKind::Count:
        break;
    }
    return kDefaultIcon;
}

void AssetIconResolver::RegisterExtension(
    const std::string_view p_extension,
    const Icons::IconId p_icon)
{
    if (p_extension.empty() || p_icon.value.empty())
        return;
    m_extensionIcons[NormalizeExtension(std::string(p_extension))] = std::string(p_icon.value);
}

void AssetIconResolver::RegisterImporter(
    const std::string_view p_importerId,
    const Icons::IconId p_icon)
{
    if (p_importerId.empty() || p_icon.value.empty())
        return;
    m_importerIcons[NormalizeKey(p_importerId)] = std::string(p_icon.value);
}

void AssetIconResolver::ClearPluginMappings()
{
    m_extensionIcons.clear();
    m_importerIcons.clear();
}

std::string AssetIconResolver::NormalizeExtension(const std::string_view p_extension)
{
    std::string extension = ToLower(std::string(p_extension));
    if (!extension.empty() && extension.front() != '.')
        extension.insert(extension.begin(), '.');
    return extension;
}

std::string AssetIconResolver::NormalizeKey(const std::string_view p_key)
{
    return ToLower(std::string(p_key));
}
}
