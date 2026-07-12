#include <UI/GUIDrawer.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <unordered_set>

#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>
#include <Guid.h>
#include <Rendering/Assets/MeshArtifact.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Resources/Loaders/TextureLoader.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Resources/Parsers/AssimpParser.h>
#include <Rendering/Resources/Parsers/FbxSdkParser.h>
#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Settings/ETextureFilteringMode.h>
#include <Utils/PathParser.h>

#include "Core/EditorResources.h"
#include "Resources/RawTextures.h"

using namespace NLS;

#ifndef NLS_HAS_ASSIMP_FBX_IMPORTER
#define NLS_HAS_ASSIMP_FBX_IMPORTER 0
#endif

#ifndef NLS_HAS_AUTODESK_FBX_SDK
#define NLS_HAS_AUTODESK_FBX_SDK 0
#endif

namespace
{
    using Clock = std::chrono::steady_clock;

    double MillisecondsSince(const Clock::time_point& start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    void LogEditorResourceLoad(const std::string& category, const std::string& id, const Clock::time_point& start)
    {
        NLS_LOG_INFO(
            "[EditorResources] Loaded " + category + " \"" + id + "\" in " +
            std::to_string(MillisecondsSince(start)) + " ms");
    }

    std::optional<std::filesystem::path> ResolveProjectLibraryRoot(const std::string& projectAssetsPath)
    {
        if (projectAssetsPath.empty())
            return std::nullopt;

        auto assetsPath = std::filesystem::path(projectAssetsPath).lexically_normal();
        while (!assetsPath.empty() && assetsPath.filename().empty())
        {
            const auto parent = assetsPath.parent_path();
            if (parent == assetsPath)
                break;
            assetsPath = parent;
        }

        const auto projectRoot = assetsPath.filename() == "Assets" ? assetsPath.parent_path() : assetsPath;
        if (projectRoot.empty())
            return std::nullopt;

        return projectRoot / "Library";
    }

    std::filesystem::path StripTrailingEmptyFilename(std::filesystem::path path)
    {
        path = path.lexically_normal();
        while (!path.empty() && !path.has_filename())
            path = path.parent_path();
        return path;
    }

    template<typename Parser>
    bool LoadEditorHelperMeshData(
        Parser& parser,
        const std::filesystem::path& sourcePath,
        std::vector<NLS::Render::Resources::Parsers::ParsedMeshData>& meshes,
        std::vector<std::string>& materials)
    {
        return parser.LoadModelData(
            sourcePath.string(),
            meshes,
            materials,
            NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE |
                NLS::Render::Resources::Parsers::EModelParserFlags::GLOBAL_SCALE,
            nullptr,
            true);
    }

    bool GenerateEditorHelperMeshArtifact(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& artifactPath)
    {
        std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
        std::vector<std::string> materials;
        bool loaded = false;
#if NLS_HAS_ASSIMP_FBX_IMPORTER
        {
            NLS::Render::Resources::Parsers::AssimpParser parser;
            loaded = LoadEditorHelperMeshData(parser, sourcePath, meshes, materials);
        }
#endif
#if NLS_HAS_AUTODESK_FBX_SDK
        if (!loaded)
        {
            NLS::Render::Resources::Parsers::FbxSdkParser parser;
            loaded = LoadEditorHelperMeshData(parser, sourcePath, meshes, materials);
        }
#endif
        if (!loaded || meshes.size() != 1u)
        {
            NLS_LOG_WARNING(
                "[EditorResources] Failed to generate editor helper mesh artifact from " +
                sourcePath.string());
            return false;
        }

        std::error_code error;
        std::filesystem::create_directories(artifactPath.parent_path(), error);
        if (error)
            return false;

        NLS::Render::Assets::MeshArtifactData artifact;
        artifact.vertices = meshes.front().vertices;
        artifact.indices = meshes.front().indices;
        artifact.materialIndex = meshes.front().materialIndex;
        const auto bytes = NLS::Render::Assets::SerializeMeshArtifact(artifact);
        if (bytes.empty())
            return false;

        std::ofstream output(artifactPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;

        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return output.good();
    }

    std::optional<std::filesystem::path> EnsureEditorHelperMeshArtifact(
        const std::string& projectAssetsPath,
        const std::string& meshId,
        const std::string& sourcePath)
    {
        const auto libraryRoot = ResolveProjectLibraryRoot(projectAssetsPath);
        if (!libraryRoot.has_value())
            return std::nullopt;

        const auto resourcePath =
            *libraryRoot /
            "EditorHelperArtifacts" /
            "Models" /
            NLS::Guid::NewDeterministic("EditorHelperMeshArtifact:" + meshId).ToString();
        std::error_code error;
        if (std::filesystem::is_regular_file(resourcePath, error))
            return resourcePath;

        if (GenerateEditorHelperMeshArtifact(sourcePath, resourcePath))
            return resourcePath;

        return std::nullopt;
    }

}

Editor::Core::EditorResources::EditorResources(
    const std::string& p_editorAssetsPath,
    const std::string& p_projectAssetsPath)
{
    using namespace NLS::Render::Resources::Loaders;
    const auto constructorStart = Clock::now();
    m_projectAssetsPath = p_projectAssetsPath;

    m_catalog = EditorResourceCatalog::CreateDefault();
    const auto editorAssetsPath = StripTrailingEmptyFilename(std::filesystem::path(p_editorAssetsPath));
    m_catalog.SetDevelopmentAssetsRoot(editorAssetsPath.parent_path());

    const auto firstFilterEditor = NLS::Render::Settings::ETextureFilteringMode::LINEAR;
    const auto secondFilterEditor = NLS::Render::Settings::ETextureFilteringMode::LINEAR;
    const auto firstFilterBillboard = NLS::Render::Settings::ETextureFilteringMode::NEAREST;
    const auto secondFilterBillboard = NLS::Render::Settings::ETextureFilteringMode::NEAREST;

    {
        std::vector<uint64_t> raw = BUTTON_PLAY;
        m_textures["Button_Play"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 64, 64, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = BUTTON_PAUSE;
        m_textures["Button_Pause"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 64, 64, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = BUTTON_STOP;
        m_textures["Button_Stop"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 64, 64, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = BUTTON_NEXT;
        m_textures["Button_Next"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 64, 64, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = BUTTON_REFRESH;
        m_textures["Button_Refresh"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 64, 64, firstFilterEditor, secondFilterEditor, false);
    }

    const auto loadEditorIcon = [&](const std::string& id)
    {
        const auto filePath = ResolveDevelopmentPath(id);
        if (!filePath.has_value())
        {
            NLS_LOG_WARNING("[EditorResources] Missing catalog path for texture resource \"" + id + "\"");
            return;
        }

        auto* texture = TextureLoader::CreateFromImageFile(filePath->string(), firstFilterEditor, secondFilterEditor, false);
        if (texture == nullptr)
        {
            NLS_LOG_WARNING(
                "[EditorResources] Failed to load texture resource \"" + id +
                "\" from " + filePath->string());
            return;
        }

        if (const auto existing = m_textures.find(id); existing != m_textures.end())
            TextureLoader::Destroy(existing->second);
        m_textures[id] = texture;
    };

    {
        std::vector<uint64_t> raw = ICON_FILE;
        m_textures["editor.icon.asset.default"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_FOLDER;
        m_textures["editor.icon.asset.folder"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_TEXTURE;
        m_textures["editor.icon.asset.texture"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_MODEL;
        m_textures["editor.icon.asset.mesh"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_MATERIAL;
        m_textures["editor.icon.asset.material"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_SOUND;
        m_textures["editor.icon.asset.audio"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_SCRIPT;
        m_textures["editor.icon.asset.script"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_FONT;
        m_textures["editor.icon.asset.font"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }

    for (const auto& record : m_catalog.GetRecords())
    {
        if (record.type == EditorResourceType::Icon)
            loadEditorIcon(record.id);
    }

    {
        std::vector<uint64_t> raw = BILL_PLIGHT;
        m_textures["Bill_Point_Light"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 128, 128, firstFilterBillboard, secondFilterBillboard, false);
    }
    {
        std::vector<uint64_t> raw = BILL_SLIGHT;
        m_textures["Bill_Spot_Light"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 128, 128, firstFilterBillboard, secondFilterBillboard, false);
    }
    {
        std::vector<uint64_t> raw = BILL_DLIGHT;
        m_textures["Bill_Directional_Light"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 128, 128, firstFilterBillboard, secondFilterBillboard, false);
    }
    {
        std::vector<uint64_t> raw = BILL_ABLIGHT;
        m_textures["Bill_Ambient_Box_Light"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 128, 128, firstFilterBillboard, secondFilterBillboard, false);
    }
    {
        std::vector<uint64_t> raw = BILL_ASLIGHT;
        m_textures["Bill_Ambient_Sphere_Light"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 128, 128, firstFilterBillboard, secondFilterBillboard, false);
    }

    m_meshPaths["Cylinder"] = "editor.model.helper.cylinder";
    m_meshPaths["Plane"] = "editor.model.helper.plane";
    m_meshPaths["Vertical_Plane"] = "editor.model.helper.vertical-plane";
    m_meshPaths["Roll"] = "editor.model.helper.roll";
    m_meshPaths["Sphere"] = "editor.model.helper.sphere";
    m_meshPaths["Camera"] = "editor.model.helper.camera";

    m_shaderPaths["Grid"] = "editor.shader.grid";
    m_shaderPaths["Billboard"] = "editor.shader.billboard";
    m_shaderPaths["DebugLitColor"] = "editor.shader.debug-lit-color";
    m_shaderPaths["SelectionOutlineMask"] = "editor.shader.selection-outline-mask";
    m_shaderPaths["SelectionOutlineComposite"] = "editor.shader.selection-outline-composite";

    {
        std::vector<uint64_t> raw = EMPTY_TEXTURE;
        m_textures["Empty_Texture"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 64, 64, firstFilterEditor, secondFilterEditor, false);
        UI::GUIDrawer::ProvideEmptyTexture(*m_textures["Empty_Texture"]);
    }

    NLS_LOG_INFO(
        "[EditorResources] Startup preload finished in " +
        std::to_string(MillisecondsSince(constructorStart)) + " ms");
}

Editor::Core::EditorResources::~EditorResources()
{
    std::unordered_set<NLS::Render::Resources::Texture2D*> destroyedTextures;
    for (auto [id, texture] : m_textures)
    {
        if (destroyedTextures.insert(texture).second)
            NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture);
    }

    for (auto [id, shader] : m_shaders)
        NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader);
}

NLS::Render::Resources::Texture2D* Editor::Core::EditorResources::GetFileIcon(const std::string& p_filename)
{
    return GetTexture(GetFileIconId(p_filename));
}

const char* Editor::Core::EditorResources::GetFileIconId(const std::string& p_filename)
{
    using namespace Utils;
    const auto fileType = PathParser::GetFileType(p_filename);
    switch (fileType)
    {
    case PathParser::EFileType::TEXTURE:
        return "editor.icon.asset.texture";
    case PathParser::EFileType::MODEL:
        return "editor.icon.asset.mesh";
    case PathParser::EFileType::PREFAB:
        return "editor.icon.asset.prefab";
    case PathParser::EFileType::SHADER:
        return "editor.icon.asset.shader";
    case PathParser::EFileType::MATERIAL:
        return "editor.icon.asset.material";
    case PathParser::EFileType::SCENE:
        return "editor.icon.asset.scene";
    case PathParser::EFileType::SOUND:
        return "editor.icon.asset.audio";
    case PathParser::EFileType::SCRIPT:
        return "editor.icon.asset.script";
    case PathParser::EFileType::FONT:
        return "editor.icon.asset.font";
    default:
        return "editor.icon.asset.default";
    }
}

NLS::Render::Resources::Texture2D* Editor::Core::EditorResources::GetTexture(const std::string& p_id)
{
    if (const auto found = m_textures.find(p_id); found != m_textures.end())
        return found->second;

    return nullptr;
}

NLS::Render::Resources::Mesh* Editor::Core::EditorResources::GetMesh(const std::string& p_id)
{
    if (const auto found = m_meshes.find(p_id); found != m_meshes.end())
        return found->second;

    return LoadMesh(p_id);
}

NLS::Render::Resources::Shader* Editor::Core::EditorResources::GetShader(const std::string& p_id)
{
    if (const auto found = m_shaders.find(p_id); found != m_shaders.end())
        return found->second;

    return LoadShader(p_id);
}

NLS::Render::Resources::Shader* Editor::Core::EditorResources::GetLoadedShader(const std::string& p_id) const
{
    const auto found = m_shaders.find(p_id);
    return found != m_shaders.end() ? found->second : nullptr;
}

std::optional<std::filesystem::path> Editor::Core::EditorResources::ResolveDevelopmentPath(const std::string& p_id) const
{
    return m_catalog.ResolvePath(p_id, EditorResourceBackendMode::Development);
}

void Editor::Core::EditorResources::PreloadStartupResources()
{
    GetShader("Grid");
    GetShader("Billboard");
    GetShader("DebugLitColor");

    GetMesh("Plane");
    GetMesh("Vertical_Plane");
    GetMesh("Camera");
}

NLS::Render::Resources::Mesh* Editor::Core::EditorResources::LoadMesh(const std::string& p_id)
{
    const auto found = m_meshPaths.find(p_id);
    if (found == m_meshPaths.end())
        return nullptr;

    const auto sourcePath = ResolveDevelopmentPath(found->second);
    if (!sourcePath.has_value())
    {
        NLS_LOG_WARNING("[EditorResources] Missing catalog path for mesh resource \"" + found->second + "\"");
        return nullptr;
    }

    const auto loadStart = Clock::now();
    auto& meshManager = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MeshManager>();
    const auto resourcePath = EnsureEditorHelperMeshArtifact(m_projectAssetsPath, p_id, sourcePath->string());
    auto* mesh = resourcePath.has_value()
        ? meshManager.GetResource(resourcePath->string(), true)
        : nullptr;
    if (mesh != nullptr)
    {
        m_meshes[p_id] = mesh;
        LogEditorResourceLoad("mesh", p_id, loadStart);
    }

    return mesh;
}

NLS::Render::Resources::Shader* Editor::Core::EditorResources::LoadShader(const std::string& p_id)
{
    const auto found = m_shaderPaths.find(p_id);
    if (found == m_shaderPaths.end())
        return nullptr;

    const auto shaderPath = ResolveDevelopmentPath(found->second);
    if (!shaderPath.has_value())
    {
        NLS_LOG_WARNING("[EditorResources] Missing catalog path for shader resource \"" + found->second + "\"");
        return nullptr;
    }

    const auto loadStart = Clock::now();
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(shaderPath->string(), m_projectAssetsPath);
    if (shader != nullptr)
    {
        m_shaders[p_id] = shader;
        LogEditorResourceLoad("shader", p_id, loadStart);
    }

    return shader;
}
