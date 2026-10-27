#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "Assets/AssetBrowserPresentation.h"
#include "Icons/EditorIconCatalog.h"

namespace NLS::Editor::Assets
{
    /**
    * The single authority for asset type -> icon id. Browser rows, file icons
    * and thumbnail fallbacks all delegate here; only this class owns the table.
    *
    * Priority for path-based lookups:
    *   plugin extension mapping
    *   -> language-specific extension (.cs/.lua)
    *   -> Core::Assets::InferAssetType(path)
    *   -> editor.icon.asset.default
    */
    class AssetIconResolver
    {
    public:
        static AssetIconResolver& Default();

        Icons::IconId ResolveAssetType(NLS::Core::Assets::AssetType p_type) const;
        Icons::IconId ResolvePath(std::string_view p_path) const;
        Icons::IconId ResolveImporter(std::string_view p_importerId) const;
        Icons::IconId ResolveBrowserType(AssetBrowserItemType p_type) const;
        Icons::IconId ResolveThumbnailFallback(
            AssetThumbnailKind p_kind,
            std::string_view p_sourcePath = {}) const;

        // Plugins may override extension/importer mappings without touching the
        // Runtime AssetType enum. Keys are case-insensitive; extensions may be
        // passed with or without a leading dot.
        void RegisterExtension(std::string_view p_extension, Icons::IconId p_icon);
        void RegisterImporter(std::string_view p_importerId, Icons::IconId p_icon);
        void ClearPluginMappings();

        // Null-terminated id for legacy APIs that return const char*.
        static const char* IconIdCString(Icons::IconId p_icon) { return p_icon.value.data(); }

    private:
        static std::string NormalizeExtension(std::string_view p_extension);
        static std::string NormalizeKey(std::string_view p_key);

        std::unordered_map<std::string, std::string> m_extensionIcons;
        std::unordered_map<std::string, std::string> m_importerIcons;
    };
}
