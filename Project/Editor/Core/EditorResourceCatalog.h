#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Platform/Process/Process.h>

namespace NLS::Editor::Core
{
    enum class EditorResourceBackendMode
    {
        Development,
        Packaged
    };

    enum class EditorResourceType
    {
        Brand,
        Font,
        Icon,
        HelperModel,
        Shader,
        Layout,
        Generated,
        Preview
    };

    enum class EditorResourceScope
    {
        Editor,
        RuntimeBuiltin,
        ProjectUser,
        Generated
    };

    struct EditorResourceRecord
    {
        std::string id;
        EditorResourceType type = EditorResourceType::Icon;
        EditorResourceScope scope = EditorResourceScope::Editor;
        std::filesystem::path developmentPath;
        std::filesystem::path packagedPath;
    };

    using EditorResourceRoots = NLS::Platform::Process::InstallResourceRoots;

    class EditorResourceCatalog
    {
    public:
        explicit EditorResourceCatalog(std::filesystem::path executablePath = {});

        static std::filesystem::path ResolveExecutablePath();
        static EditorResourceRoots ResolveRootsFromExecutable(const std::filesystem::path& executablePath);
        static EditorResourceCatalog CreateDefault(std::filesystem::path executablePath = {});
        static const std::vector<EditorResourceRecord>& DefaultRecords();

        void SetDevelopmentAssetsRoot(std::filesystem::path assetsRoot);
        bool AddRecord(EditorResourceRecord record);
        void AddDefaultRecords();
        bool Contains(std::string_view id) const;

        std::optional<EditorResourceRecord> FindRecord(std::string_view id) const;
        std::optional<std::filesystem::path> ResolvePath(
            std::string_view id,
            EditorResourceBackendMode mode = EditorResourceBackendMode::Development) const;

        const EditorResourceRoots& GetRoots() const { return m_roots; }
        const std::vector<EditorResourceRecord>& GetRecords() const { return m_records; }

    private:
        static bool IsContainedRelativePath(const std::filesystem::path& path);
        static std::filesystem::path NormalizePath(std::filesystem::path path);

        EditorResourceRoots m_roots;
        std::vector<EditorResourceRecord> m_records;
        std::unordered_map<std::string, size_t> m_indexById;
    };
}
