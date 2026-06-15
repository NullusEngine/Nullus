#include <filesystem>
#include <Utils/PathParser.h>
#include <Utils/SizeConverter.h>

#include <fstream>
#include <optional>

#include <Json/json.hpp>

#include <UI/GUIDrawer.h>
#include <ServiceLocator.h>
#include <ResourceManagement/MeshManager.h>
#include <ResourceManagement/MaterialManager.h>
#include <ResourceManagement/TextureManager.h>

#include <Debug/Logger.h>

#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Layout/GroupCollapsable.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Layout/NewLine.h>
#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Selection/ComboBox.h>

#include "Panels/AssetProperties.h"
#include "Panels/AssetBrowser.h"
#include "Panels/AssetView.h"
#include "Assets/AssetMeta.h"
#include "Assets/EditorAssetManifestJson.h"
#include "Assets/AssetImporterFacade.h"
#include "Assets/EditorAssetPath.h"
#include "Core/EditorActions.h"
using namespace NLS;

namespace
{
std::filesystem::path ProjectRootFromAssetsPath(const std::string& projectAssetsPath)
{
    auto assetsPath = std::filesystem::path(projectAssetsPath).lexically_normal();
    while (!assetsPath.empty() && !assetsPath.has_filename())
        assetsPath = assetsPath.parent_path();
    return assetsPath.parent_path();
}

std::string ToEditorAssetPathFromResource(const std::string& resource)
{
    if (resource.empty() || resource.front() == ':')
        return {};

    const auto normalizedResource = std::filesystem::path(resource).lexically_normal();
    if (!normalizedResource.empty() && *normalizedResource.begin() == "Assets")
        return NLS::Editor::Assets::NormalizeEditorAssetPath(normalizedResource);

    return NLS::Editor::Assets::NormalizeEditorAssetPath(
        std::filesystem::path("Assets") / normalizedResource);
}

struct AssetPropertiesTarget
{
    std::string resourcePath;
    std::string sourceResourcePath;
    std::string sourceAssetPath;
    std::string subAssetKey;
};

struct AssetPropertiesSubAssetInfo
{
    NLS::Core::Assets::AssetId assetId;
    NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    std::string artifactPath;
};

AssetPropertiesTarget ParseAssetPropertiesTarget(const std::string& resource)
{
    AssetPropertiesTarget target;
    target.resourcePath = resource;
    target.sourceResourcePath = resource;

    if (const auto delimiter = target.sourceResourcePath.find('#');
        delimiter != std::string::npos)
    {
        target.subAssetKey = target.sourceResourcePath.substr(delimiter + 1u);
        target.sourceResourcePath = target.sourceResourcePath.substr(0u, delimiter);
    }

    target.sourceAssetPath = ToEditorAssetPathFromResource(target.sourceResourcePath);
    return target;
}

const char* AssetPropertiesArtifactTypeLabel(const NLS::Core::Assets::ArtifactType type)
{
    using NLS::Core::Assets::ArtifactType;
    switch (type)
    {
    case ArtifactType::Model: return "Model";
    case ArtifactType::Mesh: return "Mesh";
    case ArtifactType::Material: return "Material";
    case ArtifactType::Texture: return "Texture";
    case ArtifactType::Skeleton: return "Skeleton";
    case ArtifactType::Skin: return "Skin";
    case ArtifactType::AnimationClip: return "Animation";
    case ArtifactType::MorphTarget: return "Morph Target";
    case ArtifactType::Prefab: return "Prefab";
    case ArtifactType::Scene: return "Scene";
    case ArtifactType::Shader: return "Shader";
    case ArtifactType::Audio: return "Audio";
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        break;
    }
    return "Unknown";
}

std::filesystem::path RealPathForAssetPropertiesTarget(const std::string& resource)
{
    return std::filesystem::path(EDITOR_EXEC(GetRealPath(ParseAssetPropertiesTarget(resource).sourceResourcePath)));
}

std::optional<AssetPropertiesSubAssetInfo> ReadAssetPropertiesSubAssetInfo(
    const AssetPropertiesTarget& target,
    const std::filesystem::path& projectRoot)
{
    if (target.sourceAssetPath.empty() || target.subAssetKey.empty() || projectRoot.empty())
        return std::nullopt;

    const auto sourcePath = NLS::Editor::Assets::ResolveEditorAssetPath(
        NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot),
        target.sourceAssetPath);
    if (sourcePath.empty())
        return std::nullopt;

    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(sourcePath));
    if (!meta.has_value() || !meta->id.IsValid())
        return std::nullopt;

    const auto manifestPath =
        projectRoot /
        "Library" /
        "Artifacts" /
        meta->id.ToString() /
        "manifest.json";
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input)
        return std::nullopt;

    auto root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded())
        return std::nullopt;

    const auto manifest = NLS::Editor::Assets::ParseArtifactManifestJson(root, true);
    if (!manifest.has_value())
        return std::nullopt;

    const auto* artifact = manifest->FindSubAsset(target.subAssetKey);
    if (artifact == nullptr)
        return std::nullopt;

    return AssetPropertiesSubAssetInfo {
        meta->id,
        artifact->artifactType,
        artifact->artifactPath
    };
}
}

Editor::Panels::AssetProperties::AssetProperties
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) :
	PanelWindow(p_title, p_opened, p_windowSettings)
{
    m_targetChanged += [this]() { SetTarget(m_assetSelector->content); };

	CreateHeaderButtons();

    m_headerSeparator = &CreateWidget<UI::Widgets::Separator>();
    m_headerSeparator->enabled = false;

    CreateAssetSelector();

    m_settings = &CreateWidget<UI::Widgets::GroupCollapsable>("Settings");
	m_settingsColumns = &m_settings->CreateWidget<UI::Widgets::Columns>(2);
	m_settingsColumns->widths[0] = 150;

    m_info = &CreateWidget<UI::Widgets::GroupCollapsable>("Info");
    m_infoColumns = &m_info->CreateWidget<UI::Widgets::Columns>(2);
    m_infoColumns->widths[0] = 150;

    m_settings->enabled = m_info->enabled = false;
}

void Editor::Panels::AssetProperties::SetTarget(const std::string& p_path)
{
    if (p_path.empty())
    {
        m_resource.clear();
    }
    else
    {
        const auto delimiter = p_path.find('#');
        const auto sourcePath = delimiter == std::string::npos
            ? p_path
            : p_path.substr(0u, delimiter);
        m_resource = EDITOR_EXEC(GetResourcePath(sourcePath));
        if (delimiter != std::string::npos)
            m_resource += p_path.substr(delimiter);
    }

    if (m_assetSelector)
    {
        m_assetSelector->content = m_resource;
    }

    Refresh();
}

void Editor::Panels::AssetProperties::Refresh()
{
    const auto target = ParseAssetPropertiesTarget(m_resource);
    m_metadata.reset(new Filesystem::IniFile(EDITOR_EXEC(GetRealPath(target.sourceResourcePath)) + ".meta"));

    CreateSettings();
    CreateInfo();

    m_applyButton->enabled = m_settings->enabled;
    m_resetButton->enabled = m_settings->enabled;
    m_revertButton->enabled = m_settings->enabled;
    m_reimportButton->enabled = false;

    switch (Utils::PathParser::GetFileType(target.sourceResourcePath))
    {
    case Utils::PathParser::EFileType::MODEL:
    case Utils::PathParser::EFileType::TEXTURE:
    case Utils::PathParser::EFileType::MATERIAL:
        m_previewButton->enabled = true;
        break;
    default:
        m_previewButton->enabled = false;
        break;
    }

    if (target.subAssetKey.empty() &&
        Utils::PathParser::GetFileType(target.sourceResourcePath) == Utils::PathParser::EFileType::MODEL &&
        !target.sourceResourcePath.empty() &&
        target.sourceResourcePath.front() != ':')
    {
        m_reimportButton->enabled = true;
    }

    // Enables the header separator (And the line break) if at least one button is enabled
    m_headerSeparator->enabled = m_applyButton->enabled || m_reimportButton->enabled || m_resetButton->enabled || m_revertButton->enabled || m_previewButton->enabled;
    m_headerLineBreak->enabled = m_headerSeparator->enabled;
}

void Editor::Panels::AssetProperties::Preview()
{
	auto& assetView = EDITOR_PANEL(Editor::Panels::AssetView, "Asset View");

    const auto target = ParseAssetPropertiesTarget(m_resource);
	const auto fileType = Utils::PathParser::GetFileType(target.sourceResourcePath);

	if (fileType == Utils::PathParser::EFileType::MODEL)
	{
		if (auto resource = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager).GetResource(target.sourceResourcePath))
		{
			assetView.SetResource(resource);
		}
	}
	else if (fileType == Utils::PathParser::EFileType::MATERIAL)
	{
		if (auto resource = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager).GetResource(target.sourceResourcePath))
		{
			assetView.SetResource(resource);
		}
	}
	else if (fileType == Utils::PathParser::EFileType::TEXTURE)
	{
		if (auto resource = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager).GetResource(target.sourceResourcePath))
		{
			assetView.SetResource(resource);
		}
	}

	assetView.Open();
}

void Editor::Panels::AssetProperties::CreateHeaderButtons()
{
	m_applyButton = &CreateWidget<UI::Widgets::Button>("Apply");
    m_applyButton->idleBackgroundColor = { 0.23f, 0.49f, 0.82f };
    m_applyButton->hoveredBackgroundColor = { 0.29f, 0.58f, 0.93f };
    m_applyButton->clickedBackgroundColor = { 0.18f, 0.41f, 0.71f };
    m_applyButton->textColor = { 0.96f, 0.97f, 0.99f };
    m_applyButton->enabled = false;
    m_applyButton->lineBreak = false;
    m_applyButton->ClickedEvent += std::bind(&AssetProperties::Apply, this);

    m_reimportButton = &CreateWidget<UI::Widgets::Button>("Reimport");
    m_reimportButton->idleBackgroundColor = { 0.18f, 0.19f, 0.21f };
    m_reimportButton->hoveredBackgroundColor = { 0.23f, 0.24f, 0.26f };
    m_reimportButton->clickedBackgroundColor = { 0.26f, 0.28f, 0.31f };
    m_reimportButton->enabled = false;
    m_reimportButton->lineBreak = false;
    m_reimportButton->ClickedEvent += std::bind(&AssetProperties::Reimport, this);

	m_revertButton = &CreateWidget<UI::Widgets::Button>("Revert");
	m_revertButton->idleBackgroundColor = { 0.18f, 0.19f, 0.21f };
    m_revertButton->hoveredBackgroundColor = { 0.23f, 0.24f, 0.26f };
    m_revertButton->clickedBackgroundColor = { 0.26f, 0.28f, 0.31f };
    m_revertButton->enabled = false;
    m_revertButton->lineBreak = false;
    m_revertButton->ClickedEvent += std::bind(&AssetProperties::SetTarget, this, m_resource);

	m_previewButton = &CreateWidget<UI::Widgets::Button>("Preview");
	m_previewButton->idleBackgroundColor = { 0.18f, 0.19f, 0.21f };
    m_previewButton->hoveredBackgroundColor = { 0.23f, 0.24f, 0.26f };
    m_previewButton->clickedBackgroundColor = { 0.26f, 0.28f, 0.31f };
    m_previewButton->enabled = false;
	m_previewButton->lineBreak = false;
	m_previewButton->ClickedEvent += std::bind(&AssetProperties::Preview, this);

	m_resetButton = &CreateWidget<UI::Widgets::Button>("Reset to default");
	m_resetButton->idleBackgroundColor = { 0.39f, 0.16f, 0.18f };
    m_resetButton->hoveredBackgroundColor = { 0.47f, 0.20f, 0.22f };
    m_resetButton->clickedBackgroundColor = { 0.33f, 0.13f, 0.15f };
    m_resetButton->textColor = { 0.96f, 0.97f, 0.99f };
    m_resetButton->enabled = false;
    m_resetButton->lineBreak = false;
	m_resetButton->ClickedEvent += [this]
	{
		m_metadata->RemoveAll();
		CreateSettings();
	};

    m_headerLineBreak = &CreateWidget<UI::Widgets::NewLine>();
    m_headerLineBreak->enabled = false;
}

void Editor::Panels::AssetProperties::CreateAssetSelector()
{
    auto& columns = CreateWidget<UI::Widgets::Columns>(2);
    columns.widths[0] = 150;
    m_assetSelector = &NLS::UI::GUIDrawer::DrawAsset(columns, "Target", m_resource, &m_targetChanged);
}

void Editor::Panels::AssetProperties::CreateSettings()
{
	m_settingsColumns->RemoveAllWidgets();

    const auto target = ParseAssetPropertiesTarget(m_resource);
	const auto fileType = Utils::PathParser::GetFileType(target.sourceResourcePath);

    m_settings->enabled = target.subAssetKey.empty();

	if (target.subAssetKey.empty() && fileType == Utils::PathParser::EFileType::MODEL)
	{
		CreateModelSettings();
	}
	else if (target.subAssetKey.empty() && fileType == Utils::PathParser::EFileType::TEXTURE)
	{
		CreateTextureSettings();
	}
    else
    {
        m_settings->enabled = false;
    }
}

void Editor::Panels::AssetProperties::CreateInfo()
{
    const auto target = ParseAssetPropertiesTarget(m_resource);
    const auto realPath = EDITOR_EXEC(GetRealPath(target.sourceResourcePath));

    m_infoColumns->RemoveAllWidgets();

    if (std::filesystem::exists(realPath))
    {
        m_info->enabled = true;

        NLS::UI::GUIDrawer::CreateTitle(*m_infoColumns, "Path");
        m_infoColumns->CreateWidget<UI::Widgets::Text>(realPath);

        NLS::UI::GUIDrawer::CreateTitle(*m_infoColumns, "Size");
        const auto [size, unit] = Utils::SizeConverter::ConvertToOptimalUnit(static_cast<float>(std::filesystem::file_size(realPath)), Utils::SizeConverter::ESizeUnit::BYTE);
        m_infoColumns->CreateWidget<UI::Widgets::Text>(std::to_string(size) + " " + Utils::SizeConverter::UnitToString(unit));

        NLS::UI::GUIDrawer::CreateTitle(*m_infoColumns, "Metadata");
        m_infoColumns->CreateWidget<UI::Widgets::Text>(std::filesystem::exists(realPath + ".meta") ? "Yes" : "No");

        if (!target.subAssetKey.empty())
        {
            NLS::UI::GUIDrawer::CreateTitle(*m_infoColumns, "Sub Asset");
            m_infoColumns->CreateWidget<UI::Widgets::Text>(target.subAssetKey);

            if (const auto record = ReadAssetPropertiesSubAssetInfo(
                    target,
                    ProjectRootFromAssetsPath(EDITOR_EXEC(GetContext()).projectAssetsPath));
                record.has_value())
            {
                NLS::UI::GUIDrawer::CreateTitle(*m_infoColumns, "Asset GUID");
                m_infoColumns->CreateWidget<UI::Widgets::Text>(record->assetId.ToString());

                NLS::UI::GUIDrawer::CreateTitle(*m_infoColumns, "Artifact Type");
                m_infoColumns->CreateWidget<UI::Widgets::Text>(
                    AssetPropertiesArtifactTypeLabel(record->artifactType));

                NLS::UI::GUIDrawer::CreateTitle(*m_infoColumns, "Artifact");
                m_infoColumns->CreateWidget<UI::Widgets::Text>(record->artifactPath);
            }
            else
            {
                NLS::UI::GUIDrawer::CreateTitle(*m_infoColumns, "Artifact");
                m_infoColumns->CreateWidget<UI::Widgets::Text>("Not current");
            }
        }
    }
    else
    {
        m_info->enabled = false;
    }
}

#define MODEL_FLAG_ENTRY(setting) NLS::UI::GUIDrawer::DrawBoolean(*m_settingsColumns, setting, [&]() { return m_metadata->Get<bool>(setting); }, [&](bool value) { m_metadata->Set<bool>(setting, value); })

void Editor::Panels::AssetProperties::CreateModelSettings()
{
	m_metadata->Add("CALC_TANGENT_SPACE", true);
	m_metadata->Add("JOIN_IDENTICAL_VERTICES", true);
	m_metadata->Add("MAKE_LEFT_HANDED", false);
	m_metadata->Add("TRIANGULATE", true);
	m_metadata->Add("REMOVE_COMPONENT", false);
	m_metadata->Add("GEN_NORMALS", false);
	m_metadata->Add("GEN_SMOOTH_NORMALS", true);
	m_metadata->Add("SPLIT_LARGE_MESHES", false);
	m_metadata->Add("PRE_TRANSFORM_VERTICES", true);
	m_metadata->Add("LIMIT_BONE_WEIGHTS", false);
	m_metadata->Add("VALIDATE_DATA_STRUCTURE", false);
	m_metadata->Add("IMPROVE_CACHE_LOCALITY", true);
	m_metadata->Add("REMOVE_REDUNDANT_MATERIALS", false);
	m_metadata->Add("FIX_INFACING_NORMALS", false);
	m_metadata->Add("SORT_BY_PTYPE", false);
	m_metadata->Add("FIND_DEGENERATES", false);
	m_metadata->Add("FIND_INVALID_DATA", true);
	m_metadata->Add("GEN_UV_COORDS", true);
	m_metadata->Add("TRANSFORM_UV_COORDS", false);
	m_metadata->Add("FIND_INSTANCES", true);
	m_metadata->Add("OPTIMIZE_MESHES", true);
	m_metadata->Add("OPTIMIZE_GRAPH", true);
	m_metadata->Add("FLIP_UVS", false);
	m_metadata->Add("FLIP_WINDING_ORDER", false);
	m_metadata->Add("SPLIT_BY_BONE_COUNT", false);
	m_metadata->Add("DEBONE", true);
	m_metadata->Add("GLOBAL_SCALE", true);
	m_metadata->Add("EMBED_TEXTURES", false);
	m_metadata->Add("FORCE_GEN_NORMALS", false);
	m_metadata->Add("DROP_NORMALS", false);
	m_metadata->Add("GEN_BOUNDING_BOXES", false);

	MODEL_FLAG_ENTRY("CALC_TANGENT_SPACE");
	MODEL_FLAG_ENTRY("JOIN_IDENTICAL_VERTICES");
	MODEL_FLAG_ENTRY("MAKE_LEFT_HANDED");
	MODEL_FLAG_ENTRY("TRIANGULATE");
	MODEL_FLAG_ENTRY("REMOVE_COMPONENT");
	MODEL_FLAG_ENTRY("GEN_NORMALS");
	MODEL_FLAG_ENTRY("GEN_SMOOTH_NORMALS");
	MODEL_FLAG_ENTRY("SPLIT_LARGE_MESHES");
	MODEL_FLAG_ENTRY("PRE_TRANSFORM_VERTICES");
	MODEL_FLAG_ENTRY("LIMIT_BONE_WEIGHTS");
	MODEL_FLAG_ENTRY("VALIDATE_DATA_STRUCTURE");
	MODEL_FLAG_ENTRY("IMPROVE_CACHE_LOCALITY");
	MODEL_FLAG_ENTRY("REMOVE_REDUNDANT_MATERIALS");
	MODEL_FLAG_ENTRY("FIX_INFACING_NORMALS");
	MODEL_FLAG_ENTRY("SORT_BY_PTYPE");
	MODEL_FLAG_ENTRY("FIND_DEGENERATES");
	MODEL_FLAG_ENTRY("FIND_INVALID_DATA");
	MODEL_FLAG_ENTRY("GEN_UV_COORDS");
	MODEL_FLAG_ENTRY("TRANSFORM_UV_COORDS");
	MODEL_FLAG_ENTRY("FIND_INSTANCES");
	MODEL_FLAG_ENTRY("OPTIMIZE_MESHES");
	MODEL_FLAG_ENTRY("OPTIMIZE_GRAPH");
	MODEL_FLAG_ENTRY("FLIP_UVS");
	MODEL_FLAG_ENTRY("FLIP_WINDING_ORDER");
	MODEL_FLAG_ENTRY("SPLIT_BY_BONE_COUNT");
	MODEL_FLAG_ENTRY("DEBONE");
	MODEL_FLAG_ENTRY("GLOBAL_SCALE");
	MODEL_FLAG_ENTRY("EMBED_TEXTURES");
	MODEL_FLAG_ENTRY("FORCE_GEN_NORMALS");
	MODEL_FLAG_ENTRY("DROP_NORMALS");
	MODEL_FLAG_ENTRY("GEN_BOUNDING_BOXES");
};

void Editor::Panels::AssetProperties::CreateTextureSettings()
{
	m_metadata->Add("MIN_FILTER", static_cast<int>(Render::Settings::ETextureFilteringMode::LINEAR_MIPMAP_LINEAR));
    m_metadata->Add("MAG_FILTER", static_cast<int>(Render::Settings::ETextureFilteringMode::LINEAR));
	m_metadata->Add("ENABLE_MIPMAPPING", true);

    std::map<int, std::string> filteringModes
    {
        {0x2600, "NEAREST"},
        {0x2601, "LINEAR"},
        {0x2700, "NEAREST_MIPMAP_NEAREST"},
        {0x2703, "LINEAR_MIPMAP_LINEAR"},
        {0x2701, "LINEAR_MIPMAP_NEAREST"},
        {0x2702, "NEAREST_MIPMAP_LINEAR"}
    };

	NLS::UI::GUIDrawer::CreateTitle(*m_settingsColumns, "MIN_FILTER");
	auto& minFilter = m_settingsColumns->CreateWidget<UI::Widgets::ComboBox>(m_metadata->Get<int>("MIN_FILTER"));
	minFilter.choices = filteringModes;
	minFilter.ValueChangedEvent += [this](int p_choice)
	{
		m_metadata->Set("MIN_FILTER", p_choice);
	};

	NLS::UI::GUIDrawer::CreateTitle(*m_settingsColumns, "MAG_FILTER");
	auto& magFilter = m_settingsColumns->CreateWidget<UI::Widgets::ComboBox>(m_metadata->Get<int>("MAG_FILTER"));
	magFilter.choices = filteringModes;
	magFilter.ValueChangedEvent += [this](int p_choice)
	{
		m_metadata->Set("MAG_FILTER", p_choice);
	};

	NLS::UI::GUIDrawer::DrawBoolean(*m_settingsColumns, "ENABLE_MIPMAPPING", [&]() { return m_metadata->Get<bool>("ENABLE_MIPMAPPING"); }, [&](bool value) { m_metadata->Set<bool>("ENABLE_MIPMAPPING", value); });
}

void Editor::Panels::AssetProperties::Apply()
{
	m_metadata->Rewrite();

    const auto target = ParseAssetPropertiesTarget(m_resource);
	const auto resourcePath = EDITOR_EXEC(GetResourcePath(target.sourceResourcePath));
	const auto fileType = Utils::PathParser::GetFileType(target.sourceResourcePath);

	if (fileType == Utils::PathParser::EFileType::MODEL)
	{
		auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
		if (meshManager.IsResourceRegistered(resourcePath))
		{
			meshManager.AResourceManager::ReloadResource(resourcePath);
		}
	}
	else if (fileType == Utils::PathParser::EFileType::TEXTURE)
	{
		auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
		if (textureManager.IsResourceRegistered(resourcePath))
		{
			textureManager.AResourceManager::ReloadResource(resourcePath);
		}
	}

    Refresh();
}

void Editor::Panels::AssetProperties::Reimport()
{
    const auto assetPath = ParseAssetPropertiesTarget(m_resource).sourceAssetPath;
    if (assetPath.empty())
    {
        NLS_LOG_ERROR("Reimport is only available for project assets.");
        return;
    }

    if (m_metadata)
        m_metadata->Rewrite();

    const auto projectRoot = ProjectRootFromAssetsPath(EDITOR_EXEC(GetContext()).projectAssetsPath);
    auto& tracker = EDITOR_EXEC(GetContext()).importProgressTracker;
    const auto resourcePath = m_resource;

    EDITOR_EXEC(TrackBackgroundTask([projectRoot, assetPath, resourcePath, &tracker]
    {
        NLS::Editor::Assets::AssetImporterFacade importer(
            NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
        const auto imported = importer.SaveAndReimport(assetPath, tracker);
        EDITOR_EXEC(DelayAction([assetPath, resourcePath, imported]
        {
            if (imported)
            {
                auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
                if (meshManager.IsResourceRegistered(resourcePath))
                    meshManager.AResourceManager::ReloadResource(resourcePath);

                EDITOR_PANEL(NLS::Editor::Panels::AssetBrowser, "Asset Browser").Refresh();
                EDITOR_PANEL(NLS::Editor::Panels::AssetProperties, "Asset Properties").Refresh();
                NLS_LOG_INFO("Reimported asset: " + assetPath);
            }
            else
            {
                NLS_LOG_ERROR("Failed to reimport asset: " + assetPath);
            }
        }));
    }));
}
