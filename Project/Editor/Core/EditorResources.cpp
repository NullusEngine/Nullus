#include <UI/GUIDrawer.h>

#include <chrono>

#include <Debug/Logger.h>
#include <Rendering/Resources/Loaders/ModelLoader.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Resources/Loaders/TextureLoader.h>
#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Settings/ETextureFilteringMode.h>
#include <Utils/PathParser.h>

#include "Core/EditorResources.h"
#include "Resources/RawTextures.h"

using namespace NLS;

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
}

Editor::Core::EditorResources::EditorResources(const std::string& p_editorAssetsPath)
{
    using namespace NLS::Render::Resources::Loaders;
    const auto constructorStart = Clock::now();

    const std::string buttonsFolder = p_editorAssetsPath + "Textures/Buttons/";
    const std::string iconsFolder = p_editorAssetsPath + "Textures/Icons/";
    m_modelsFolder = p_editorAssetsPath + "Models/";
    m_shadersFolder = p_editorAssetsPath + "Shaders/";

    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::GEN_SMOOTH_NORMALS;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::OPTIMIZE_MESHES;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::OPTIMIZE_GRAPH;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::FIND_INSTANCES;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::CALC_TANGENT_SPACE;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::JOIN_IDENTICAL_VERTICES;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::DEBONE;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::FIND_INVALID_DATA;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::IMPROVE_CACHE_LOCALITY;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::GEN_UV_COORDS;
    m_modelParserFlags |= NLS::Render::Resources::Parsers::EModelParserFlags::GLOBAL_SCALE;

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

    m_modelPaths["Cube"] = m_modelsFolder + "Cube.fbx";
    m_modelPaths["Cylinder"] = m_modelsFolder + "Cylinder.fbx";
    m_modelPaths["Plane"] = m_modelsFolder + "Plane.fbx";
    m_modelPaths["Vertical_Plane"] = m_modelsFolder + "Vertical_Plane.fbx";
    m_modelPaths["Roll"] = m_modelsFolder + "Roll.fbx";
    m_modelPaths["Sphere"] = m_modelsFolder + "Sphere.fbx";
    m_modelPaths["Arrow_Translate"] = m_modelsFolder + "Arrow_Translate.fbx";
    m_modelPaths["Arrow_Rotate"] = m_modelsFolder + "Arrow_Rotate.fbx";
    m_modelPaths["Arrow_Scale"] = m_modelsFolder + "Arrow_Scale.fbx";
    m_modelPaths["Arrow_Picking"] = m_modelsFolder + "Arrow_Picking.fbx";
    m_modelPaths["Camera"] = m_modelsFolder + "Camera.fbx";

    m_shaderPaths["Grid"] = m_shadersFolder + "Grid.hlsl";
    m_shaderPaths["Gizmo"] = m_shadersFolder + "Gizmo.hlsl";
    m_shaderPaths["Billboard"] = m_shadersFolder + "Billboard.hlsl";
    m_shaderPaths["DebugLitColor"] = m_shadersFolder + "DebugLitColor.hlsl";

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

    for (auto [id, mesh] : m_models)
        NLS::Render::Resources::Loaders::ModelLoader::Destroy(mesh);

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

NLS::Render::Resources::Model* Editor::Core::EditorResources::GetModel(const std::string& p_id)
{
    if (const auto found = m_models.find(p_id); found != m_models.end())
        return found->second;

    return LoadModel(p_id);
}

NLS::Render::Resources::Shader* Editor::Core::EditorResources::GetShader(const std::string& p_id)
{
    if (const auto found = m_shaders.find(p_id); found != m_shaders.end())
        return found->second;

    return LoadShader(p_id);
}

NLS::Render::Resources::Model* Editor::Core::EditorResources::LoadModel(const std::string& p_id)
{
    const auto found = m_modelPaths.find(p_id);
    if (found == m_modelPaths.end())
        return nullptr;

    const auto loadStart = Clock::now();
    auto* model = NLS::Render::Resources::Loaders::ModelLoader::Create(found->second, m_modelParserFlags);
    if (model != nullptr)
    {
        m_models[p_id] = model;
        LogEditorResourceLoad("model", p_id, loadStart);
    }

    return model;
}

NLS::Render::Resources::Shader* Editor::Core::EditorResources::LoadShader(const std::string& p_id)
{
    const auto found = m_shaderPaths.find(p_id);
    if (found == m_shaderPaths.end())
        return nullptr;

    const auto loadStart = Clock::now();
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(found->second);
    if (shader != nullptr)
    {
        m_shaders[p_id] = shader;
        LogEditorResourceLoad("shader", p_id, loadStart);
    }

    return shader;
}
