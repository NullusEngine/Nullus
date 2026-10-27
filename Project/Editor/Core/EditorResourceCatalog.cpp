#include "Core/EditorResourceCatalog.h"

#include <algorithm>
#include <system_error>
#include <utility>

#include "Icons/Generated/IconCatalogGenerated.h"

namespace NLS::Editor::Core
{
namespace
{
    EditorResourceRecord MakeRecord(
        std::string id,
        EditorResourceType type,
        EditorResourceScope scope,
        std::filesystem::path developmentPath,
        std::filesystem::path packagedPath)
    {
        EditorResourceRecord record;
        record.id = std::move(id);
        record.type = type;
        record.scope = scope;
        record.developmentPath = std::move(developmentPath);
        record.packagedPath = std::move(packagedPath);
        return record;
    }

}

EditorResourceCatalog::EditorResourceCatalog(std::filesystem::path executablePath)
    : m_roots(ResolveRootsFromExecutable(executablePath.empty() ? ResolveExecutablePath() : std::move(executablePath)))
{
}

std::filesystem::path EditorResourceCatalog::ResolveExecutablePath()
{
    return NLS::Platform::Process::GetCurrentExecutablePath();
}

EditorResourceRoots EditorResourceCatalog::ResolveRootsFromExecutable(const std::filesystem::path& executablePath)
{
    return NLS::Platform::Process::ResolveInstallResourceRoots(executablePath);
}

EditorResourceCatalog EditorResourceCatalog::CreateDefault(std::filesystem::path executablePath)
{
    EditorResourceCatalog catalog(std::move(executablePath));
    catalog.AddDefaultRecords();
    return catalog;
}

const std::vector<EditorResourceRecord>& EditorResourceCatalog::DefaultRecords()
{
    static const std::vector<EditorResourceRecord> records = []
    {
        std::vector<EditorResourceRecord> result {
            MakeRecord("editor.brand.logo.mark", EditorResourceType::Brand, EditorResourceScope::Editor,
                "Editor/Brand/NullusLogoMark.png", "editor/brand/logo-mark.png"),
            MakeRecord("engine.brand.logo.mark", EditorResourceType::Brand, EditorResourceScope::RuntimeBuiltin,
                "Engine/Brand/NullusLogoMark.png", "engine/brand/logo-mark.png"),

            MakeRecord("editor.font.ui.default", EditorResourceType::Font, EditorResourceScope::Editor,
                "Editor/Fonts/Ruda-Bold.ttf", "editor/fonts/ruda-bold.ttf"),
            MakeRecord("editor.layout.default", EditorResourceType::Layout, EditorResourceScope::Editor,
                "Editor/Settings/layout.ini", "editor/settings/layout.ini"),

            MakeRecord("editor.model.helper.cylinder", EditorResourceType::HelperModel, EditorResourceScope::Editor,
                "Editor/Models/Cylinder.fbx", "editor/models/cylinder.fbx"),
            MakeRecord("editor.model.helper.plane", EditorResourceType::HelperModel, EditorResourceScope::Editor,
                "Editor/Models/Plane.fbx", "editor/models/plane.fbx"),
            MakeRecord("editor.model.helper.vertical-plane", EditorResourceType::HelperModel, EditorResourceScope::Editor,
                "Editor/Models/Vertical_Plane.fbx", "editor/models/vertical-plane.fbx"),
            MakeRecord("editor.model.helper.roll", EditorResourceType::HelperModel, EditorResourceScope::Editor,
                "Editor/Models/Roll.fbx", "editor/models/roll.fbx"),
            MakeRecord("editor.model.helper.sphere", EditorResourceType::HelperModel, EditorResourceScope::Editor,
                "Editor/Models/Sphere.fbx", "editor/models/sphere.fbx"),
            MakeRecord("editor.model.helper.camera", EditorResourceType::HelperModel, EditorResourceScope::Editor,
                "Editor/Models/Camera.fbx", "editor/models/camera.fbx"),

            MakeRecord("editor.shader.grid", EditorResourceType::Shader, EditorResourceScope::Editor,
                "Editor/Shaders/Grid.hlsl", "editor/shaders/grid.hlsl"),
            MakeRecord("editor.shader.billboard", EditorResourceType::Shader, EditorResourceScope::Editor,
                "Editor/Shaders/Billboard.hlsl", "editor/shaders/billboard.hlsl"),
            MakeRecord("editor.shader.debug-lit-color", EditorResourceType::Shader, EditorResourceScope::Editor,
                "Editor/Shaders/DebugLitColor.hlsl", "editor/shaders/debug-lit-color.hlsl"),
            MakeRecord("editor.shader.selection-outline-mask", EditorResourceType::Shader, EditorResourceScope::Editor,
                "Editor/Shaders/SelectionOutlineMask.hlsl", "editor/shaders/selection-outline-mask.hlsl"),
            MakeRecord("editor.shader.selection-outline-composite", EditorResourceType::Shader, EditorResourceScope::Editor,
                "Editor/Shaders/SelectionOutlineComposite.hlsl", "editor/shaders/selection-outline-composite.hlsl")
        };

        // Icon records are generated from Tools/Icons/icons.manifest.json.
        auto generatedIcons = GeneratedIconRecords();
        result.insert(result.end(), generatedIcons.begin(), generatedIcons.end());
        return result;
    }();
    return records;
}

void EditorResourceCatalog::SetDevelopmentAssetsRoot(std::filesystem::path assetsRoot)
{
    m_roots.assetsRoot = NormalizePath(std::move(assetsRoot));
    m_roots.editorAssetsRoot = NormalizePath(m_roots.assetsRoot / "Editor");
    m_roots.engineAssetsRoot = NormalizePath(m_roots.assetsRoot / "Engine");
}

bool EditorResourceCatalog::AddRecord(EditorResourceRecord record)
{
    if (record.id.empty() ||
        m_indexById.find(record.id) != m_indexById.end())
    {
        return false;
    }

    if (!record.developmentPath.empty() && !IsContainedRelativePath(record.developmentPath))
        return false;
    if (!record.packagedPath.empty() && record.packagedPath.is_absolute())
        return false;

    const auto index = m_records.size();
    m_records.push_back(std::move(record));
    m_indexById.emplace(m_records.back().id, index);
    return true;
}

void EditorResourceCatalog::AddDefaultRecords()
{
    for (const auto& record : DefaultRecords())
        AddRecord(record);
}

bool EditorResourceCatalog::Contains(std::string_view id) const
{
    return m_indexById.find(std::string(id)) != m_indexById.end();
}

std::optional<EditorResourceRecord> EditorResourceCatalog::FindRecord(std::string_view id) const
{
    const auto iterator = m_indexById.find(std::string(id));
    if (iterator == m_indexById.end())
        return std::nullopt;
    return m_records[iterator->second];
}

std::optional<std::filesystem::path> EditorResourceCatalog::ResolvePath(
    std::string_view id,
    const EditorResourceBackendMode mode) const
{
    const auto record = FindRecord(id);
    if (!record.has_value())
        return std::nullopt;

    if (mode == EditorResourceBackendMode::Packaged)
    {
        if (record->packagedPath.empty())
            return std::nullopt;
        return record->packagedPath.lexically_normal();
    }

    if (record->developmentPath.empty())
        return std::nullopt;
    if (!IsContainedRelativePath(record->developmentPath))
        return std::nullopt;
    return NormalizePath(m_roots.assetsRoot / record->developmentPath);
}

bool EditorResourceCatalog::IsContainedRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute())
        return false;

    for (const auto& part : path)
    {
        if (part == "..")
            return false;
    }
    return true;
}

std::filesystem::path EditorResourceCatalog::NormalizePath(std::filesystem::path path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error)
        normalized = path.lexically_normal();
    return normalized;
}
}
