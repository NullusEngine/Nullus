#include <UI/GUIDrawer.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>

#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>
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
            *libraryRoot / "EditorHelperArtifacts" / "Models" / (meshId + ".nmesh");
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

    const std::string buttonsFolder = p_editorAssetsPath + "Textures/Buttons/";
    const std::string iconsFolder = p_editorAssetsPath + "Textures/Icons/";
    const std::string editorIconFolder = p_editorAssetsPath + "Icon/";
    m_modelsFolder = p_editorAssetsPath + "Models/";
    m_shadersFolder = p_editorAssetsPath + "Shaders/";

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

    const auto loadEditorIcon = [&](const std::string& id, const std::string& fileName)
    {
        auto* texture = TextureLoader::Create(editorIconFolder + fileName, firstFilterEditor, secondFilterEditor, false);
        if (texture != nullptr)
            m_textures[id] = texture;
    };

    loadEditorIcon("Toolbar_Move", "d_movetool.png");
    loadEditorIcon("Toolbar_Rotate", "d_rotatetool.png");
    loadEditorIcon("Toolbar_Scale", "d_scaletool.png");
    loadEditorIcon("Toolbar_Pivot", "d_toolhandlepivot.png");
    loadEditorIcon("Toolbar_Center", "d_toolhandlecenter.png");
    loadEditorIcon("Toolbar_Global", "d_toolhandleglobal.png");
    loadEditorIcon("Toolbar_Local", "d_toolhandlelocal.png");

    {
        std::vector<uint64_t> raw = ICON_FILE;
        m_textures["Icon_Unknown"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_FOLDER;
        m_textures["Icon_Folder"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_TEXTURE;
        m_textures["Icon_Texture"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_MODEL;
        m_textures["Icon_Model"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_SHADER;
        m_textures["Icon_Shader"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_MATERIAL;
        m_textures["Icon_Material"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_SCENE;
        m_textures["Icon_Scene"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_SOUND;
        m_textures["Icon_Sound"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_SCRIPT;
        m_textures["Icon_Script"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
    }
    {
        std::vector<uint64_t> raw = ICON_FONT;
        m_textures["Icon_Font"] = TextureLoader::CreateFromMemory(reinterpret_cast<uint8_t*>(raw.data()), 16, 16, firstFilterEditor, secondFilterEditor, false);
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

    m_meshPaths["Cylinder"] = m_modelsFolder + "Cylinder.fbx";
    m_meshPaths["Plane"] = m_modelsFolder + "Plane.fbx";
    m_meshPaths["Vertical_Plane"] = m_modelsFolder + "Vertical_Plane.fbx";
    m_meshPaths["Roll"] = m_modelsFolder + "Roll.fbx";
    m_meshPaths["Sphere"] = m_modelsFolder + "Sphere.fbx";
    m_meshPaths["Camera"] = m_modelsFolder + "Camera.fbx";

    m_shaderPaths["Grid"] = m_shadersFolder + "Grid.hlsl";
    m_shaderPaths["Billboard"] = m_shadersFolder + "Billboard.hlsl";
    m_shaderPaths["DebugLitColor"] = m_shadersFolder + "DebugLitColor.hlsl";
    m_shaderPaths["SelectionOutlineMask"] = m_shadersFolder + "SelectionOutlineMask.hlsl";
    m_shaderPaths["SelectionOutlineComposite"] = m_shadersFolder + "SelectionOutlineComposite.hlsl";

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
    for (auto [id, texture] : m_textures)
        NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture);

    for (auto [id, shader] : m_shaders)
        NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader);
}

NLS::Render::Resources::Texture2D* Editor::Core::EditorResources::GetFileIcon(const std::string& p_filename)
{
    using namespace Utils;
    return GetTexture("Icon_" + PathParser::FileTypeToString(PathParser::GetFileType(p_filename)));
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

void Editor::Core::EditorResources::PreloadStartupResources()
{
    GetShader("Grid");
    GetShader("Billboard");
    GetShader("DebugLitColor");
    GetShader("SelectionOutlineMask");
    GetShader("SelectionOutlineComposite");

    GetMesh("Plane");
    GetMesh("Vertical_Plane");
    GetMesh("Camera");
}

NLS::Render::Resources::Mesh* Editor::Core::EditorResources::LoadMesh(const std::string& p_id)
{
    const auto found = m_meshPaths.find(p_id);
    if (found == m_meshPaths.end())
        return nullptr;

    const auto loadStart = Clock::now();
    auto& meshManager = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MeshManager>();
    const auto resourcePath = EnsureEditorHelperMeshArtifact(m_projectAssetsPath, p_id, found->second);
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

    const auto loadStart = Clock::now();
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(found->second, m_projectAssetsPath);
    if (shader != nullptr)
    {
        m_shaders[p_id] = shader;
        LogEditorResourceLoad("shader", p_id, loadStart);
    }

    return shader;
}
