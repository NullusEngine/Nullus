#include <fstream>
#include <iostream>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <system_error>
#include <vector>

#include "ImGui/imgui.h"
#include <Json/json.hpp>

#include <UI/Widgets/Texts/TextClickable.h>
#include <UI/Widgets/Visual/Image.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Plugins/DDSource.h>
#include <UI/Plugins/DDTarget.h>
#include <UI/Plugins/ContextualMenu.h>

#include <Windowing/Dialogs/MessageBox.h>
#include <Windowing/Dialogs/SaveFileDialog.h>
#include <Windowing/Dialogs/OpenFileDialog.h>
#include <Utils/SystemCalls.h>
#include <Utils/PathParser.h>
#include <Utils/String.h>

#include <ServiceLocator.h>
#include <ResourceManagement/MeshManager.h>
#include <ResourceManagement/TextureManager.h>
#include <ResourceManagement/ShaderManager.h>

#include <Debug/Logger.h>

#include "Panels/MaterialEditor.h"
#include "Panels/AssetBrowser.h"
#include "Panels/AssetView.h"
#include "Panels/AssetProperties.h"
#include "Panels/SceneView.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetManifestJson.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/AssetMeta.h"
#include "Assets/AssetImporterFacade.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/AssetDragDropWorkflow.h"
#include "Assets/PrefabUtilityFacade.h"
#include "Core/EditorActions.h"
#include "Core/EditorResources.h"
#include "GameObject.h"
#include "SceneSystem/SceneManager.h"
#include "UI/Widgets/InputFields/InputText.h"
#include "UI/UIManager.h"

using namespace NLS;
using namespace NLS::UI;
using namespace NLS::UI::Widgets;

#define FILENAMES_CHARS Editor::Panels::AssetBrowser::__FILENAMES_CHARS

const std::string FILENAMES_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-_=+ 0123456789()[]";

std::string GetAssociatedMetaFile(const std::string& p_assetPath)
{
	return p_assetPath + ".meta";
}

std::filesystem::path ProjectRootFromAssetsFolder(const std::string& projectAssetsFolder)
{
	auto assetsPath = std::filesystem::path(projectAssetsFolder).lexically_normal();
	while (!assetsPath.empty() && !assetsPath.has_filename())
		assetsPath = assetsPath.parent_path();
	return assetsPath.parent_path();
}

std::filesystem::path EditorAssetFolderFromAbsolutePath(
	const std::string& projectAssetsFolder,
	const std::string& absoluteFolderPath)
{
	const auto projectRoot = ProjectRootFromAssetsFolder(projectAssetsFolder);
	auto relative = std::filesystem::path(absoluteFolderPath).lexically_normal().lexically_relative(projectRoot);
	if (relative.empty() || relative.is_absolute())
		return {};

	for (const auto& part : relative)
	{
		if (part == "..")
			return {};
	}

	return relative;
}

std::filesystem::path EditorAssetPathFromAbsolutePath(
	const std::string& projectAssetsFolder,
	const std::string& absolutePath)
{
	return EditorAssetFolderFromAbsolutePath(projectAssetsFolder, absolutePath);
}

std::string AssetBrowserFileStamp(const std::filesystem::path& path)
{
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error)
		return {};

	error.clear();
	const auto writeTime = std::filesystem::last_write_time(path, error);
	if (error)
		return {};

	const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
	return std::to_string(size) + ":" + std::to_string(writeTimeTicks);
}

bool ManifestDependencyStampsAreCurrent(
	const nlohmann::json& manifest,
	const std::string& projectAssetsFolder,
	const std::string& absolutePath)
{
	const auto meta = NLS::Core::Assets::AssetMeta::Load(
		NLS::Core::Assets::GetAssetMetaPath(absolutePath));
	const auto importerId = NLS::Editor::Assets::JsonString(manifest, "importerId");
	const auto importerVersion = NLS::Editor::Assets::JsonUInt(manifest, "importerVersion");
	const auto targetPlatform = NLS::Editor::Assets::JsonString(manifest, "targetPlatform");
	if (!meta.has_value() ||
		!importerId.has_value() ||
		!importerVersion.has_value() ||
		!targetPlatform.has_value() ||
		*importerId != meta->importerId ||
		*importerVersion != meta->importerVersion ||
		*targetPlatform != "editor")
	{
		return false;
	}

	const auto dependencies = manifest.find("dependencies");
	if (dependencies == manifest.end() || !dependencies->is_array())
		return false;

	const auto assetPath = NLS::Editor::Assets::NormalizeEditorAssetPath(
		EditorAssetPathFromAbsolutePath(projectAssetsFolder, absolutePath));
	const auto metaAbsolutePath = NLS::Core::Assets::GetAssetMetaPath(absolutePath);
	const auto metaPath = NLS::Editor::Assets::NormalizeEditorAssetPath(
		EditorAssetPathFromAbsolutePath(projectAssetsFolder, metaAbsolutePath.string()));
	const auto projectRoot = ProjectRootFromAssetsFolder(projectAssetsFolder);

	bool checkedAsset = false;
	bool checkedMeta = false;
	for (const auto& dependency : *dependencies)
	{
		if (!dependency.is_object())
			continue;

		const auto kind = NLS::Editor::Assets::JsonStringOrDefault(dependency, "kind");
		const auto valueText = NLS::Editor::Assets::JsonStringOrDefault(dependency, "value");
		const auto stamp = NLS::Editor::Assets::JsonStringOrDefault(dependency, "hashOrVersion");
		if (!kind.has_value() || !valueText.has_value() || !stamp.has_value())
			return false;

		const auto value = NLS::Editor::Assets::NormalizeEditorAssetPath(*valueText);
		if (*kind == "source-file-hash")
		{
			if (value == assetPath)
				checkedAsset = true;

			const auto dependencyPath = NLS::Editor::Assets::ResolveEditorManifestDependencyPath(projectRoot, value);
			if (!dependencyPath.has_value() || *stamp != AssetBrowserFileStamp(*dependencyPath))
				return false;
			continue;
		}
		if (*kind == "path-to-guid-mapping")
		{
			if (value == metaPath)
				checkedMeta = true;

			const auto dependencyPath = NLS::Editor::Assets::ResolveEditorManifestDependencyPath(projectRoot, value);
			if (!dependencyPath.has_value() || *stamp != AssetBrowserFileStamp(*dependencyPath))
				return false;
			continue;
		}
	}

	return checkedAsset && checkedMeta;
}

std::filesystem::path ResolveArtifactPathForManifest(
	const std::filesystem::path& projectRoot,
	const nlohmann::json& subAsset)
{
	const auto artifactPathText = NLS::Editor::Assets::JsonStringOrDefault(subAsset, "artifactPath");
	if (!artifactPathText.has_value() || artifactPathText->empty())
		return {};

	const auto artifactPath = std::filesystem::path(*artifactPathText);
	const auto resolvedPath = artifactPath.is_absolute()
		? artifactPath.lexically_normal()
		: (projectRoot / artifactPath).lexically_normal();

	const auto relative = resolvedPath.lexically_relative(projectRoot.lexically_normal());
	if (relative.empty() || relative.is_absolute())
		return {};

	for (const auto& part : relative)
	{
		if (part == "..")
			return {};
	}

	return resolvedPath;
}

void ReimportProjectAssetAsync(const std::string& projectAssetsFolder, const std::string& absolutePath)
{
	const auto projectRoot = ProjectRootFromAssetsFolder(projectAssetsFolder);
	const auto assetPath = EditorAssetPathFromAbsolutePath(projectAssetsFolder, absolutePath);
	if (projectRoot.empty() || assetPath.empty())
	{
		NLS_LOG_ERROR("Failed to resolve project asset path for reimport: " + absolutePath);
		return;
	}

	auto& tracker = EDITOR_CONTEXT(importProgressTracker);
	EDITOR_EXEC(TrackBackgroundTask([projectRoot, assetPath = assetPath.generic_string(), &tracker]
	{
		NLS::Editor::Assets::AssetImporterFacade importer(
			NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
		const auto imported = importer.SaveAndReimport(assetPath, tracker);
		EDITOR_EXEC(DelayAction([assetPath, imported]
		{
			EDITOR_PANEL(NLS::Editor::Panels::AssetBrowser, "Asset Browser").Refresh();
			EDITOR_PANEL(NLS::Editor::Panels::AssetProperties, "Asset Properties").Refresh();
			if (imported)
				NLS_LOG_INFO("Reimported asset: " + assetPath);
			else
				NLS_LOG_ERROR("Failed to reimport asset: " + assetPath);
		}));
	}));
}

const char* AssetPreimportReasonLabel(const NLS::Editor::Assets::AssetPreimportReason reason)
{
	using NLS::Editor::Assets::AssetPreimportReason;
	switch (reason)
	{
	case AssetPreimportReason::EditorStartup:
		return "editor startup";
	case AssetPreimportReason::FileWatcherChanged:
		return "file watcher change";
	case AssetPreimportReason::AssetCopiedOrMoved:
		return "asset copy or move";
	default:
		return "asset preimport";
	}
}

NLS::Editor::Assets::AssetPreimportReason MergeAssetPreimportReasons(
	const NLS::Editor::Assets::AssetPreimportReason current,
	const NLS::Editor::Assets::AssetPreimportReason incoming)
{
	using NLS::Editor::Assets::AssetPreimportReason;
	if (current == AssetPreimportReason::FileWatcherChanged ||
		incoming == AssetPreimportReason::FileWatcherChanged)
	{
		return AssetPreimportReason::FileWatcherChanged;
	}
	if (current == AssetPreimportReason::AssetCopiedOrMoved ||
		incoming == AssetPreimportReason::AssetCopiedOrMoved)
	{
		return AssetPreimportReason::AssetCopiedOrMoved;
	}
	return AssetPreimportReason::EditorStartup;
}

NLS::Editor::Assets::AssetPreimportRequest MergeAssetPreimportRequests(
	NLS::Editor::Assets::AssetPreimportRequest current,
	const NLS::Editor::Assets::AssetPreimportRequest& incoming)
{
	current.reason = MergeAssetPreimportReasons(current.reason, incoming.reason);
	current.changedPaths.insert(
		current.changedPaths.end(),
		incoming.changedPaths.begin(),
		incoming.changedPaths.end());
	for (auto& path : current.changedPaths)
		path = path.lexically_normal();
	std::sort(current.changedPaths.begin(), current.changedPaths.end());
	current.changedPaths.erase(
		std::unique(current.changedPaths.begin(), current.changedPaths.end()),
		current.changedPaths.end());
	return current;
}

std::optional<NLS::Editor::Assets::EditorAssetDragPayload> BuildEditorAssetDragPayloadForFile(
	const std::string& projectAssetsFolder,
	const std::string& absolutePath,
	const std::string& resourceFormatPath,
	Utils::PathParser::EFileType fileType)
{
	using namespace NLS::Editor::Assets;

	if (fileType != Utils::PathParser::EFileType::MODEL &&
		fileType != Utils::PathParser::EFileType::PREFAB &&
		fileType != Utils::PathParser::EFileType::MATERIAL &&
		fileType != Utils::PathParser::EFileType::TEXTURE &&
		fileType != Utils::PathParser::EFileType::SHADER)
	{
		return std::nullopt;
	}

	const auto meta = NLS::Core::Assets::AssetMeta::Load(
		NLS::Core::Assets::GetAssetMetaPath(absolutePath));
	if (!meta.has_value() || !meta->id.IsValid())
		return std::nullopt;

	NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
	std::string subAssetKey;
	bool imported = false;

	if (fileType == Utils::PathParser::EFileType::MODEL &&
		subAssetKey.empty())
	{
		subAssetKey = "prefab:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
		artifactType = NLS::Core::Assets::ArtifactType::Prefab;
	}

	if (fileType == Utils::PathParser::EFileType::PREFAB &&
		subAssetKey.empty())
	{
		subAssetKey = "prefab:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
		artifactType = NLS::Core::Assets::ArtifactType::Prefab;
	}

	if (fileType == Utils::PathParser::EFileType::MATERIAL &&
		subAssetKey.empty())
	{
		subAssetKey = "material:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
		artifactType = NLS::Core::Assets::ArtifactType::Material;
	}

	if (fileType == Utils::PathParser::EFileType::TEXTURE &&
		subAssetKey.empty())
	{
		subAssetKey = "texture:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
		artifactType = NLS::Core::Assets::ArtifactType::Texture;
	}

	if (fileType == Utils::PathParser::EFileType::SHADER &&
		subAssetKey.empty())
	{
		subAssetKey = "shader:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
		artifactType = NLS::Core::Assets::ArtifactType::Shader;
	}

	const auto manifestPath =
		ProjectRootFromAssetsFolder(projectAssetsFolder) / "Library" / "Artifacts" / meta->id.ToString() / "manifest.json";
	if (std::filesystem::exists(manifestPath))
	{
		std::ifstream input(manifestPath, std::ios::binary);
		const auto manifest = nlohmann::json::parse(input, nullptr, false);
		if (manifest.is_object())
		{
			const auto currentManifest = ManifestDependencyStampsAreCurrent(
				manifest,
				projectAssetsFolder,
				absolutePath);
			if (fileType == Utils::PathParser::EFileType::PREFAB ||
				fileType == Utils::PathParser::EFileType::MATERIAL ||
				fileType == Utils::PathParser::EFileType::TEXTURE ||
				fileType == Utils::PathParser::EFileType::SHADER)
			{
				auto manifestPrimaryKey = JsonString(manifest, "primarySubAssetKey");
				if (manifestPrimaryKey.has_value() && !manifestPrimaryKey->empty())
					subAssetKey = std::move(*manifestPrimaryKey);
			}

			if (const auto subAssets = manifest.find("subAssets");
				subAssets != manifest.end() && subAssets->is_array())
			{
				for (const auto& subAsset : *subAssets)
				{
					const auto subAssetKeyText = JsonString(subAsset, "subAssetKey");
					if (!subAsset.is_object() ||
						!subAssetKeyText.has_value() ||
						*subAssetKeyText != subAssetKey)
					{
						continue;
					}

					const auto resolvedArtifactPath =
						ResolveArtifactPathForManifest(ProjectRootFromAssetsFolder(projectAssetsFolder), subAsset);
					if (resolvedArtifactPath.empty() || !std::filesystem::is_regular_file(resolvedArtifactPath))
						continue;

					const auto artifactTypeText = JsonStringOrDefault(subAsset, "artifactType");
					if (!artifactTypeText.has_value())
						continue;
					if (artifactTypeText == "Prefab" || artifactTypeText == "prefab")
						artifactType = NLS::Core::Assets::ArtifactType::Prefab;
					else if (artifactTypeText == "Material" || artifactTypeText == "material")
						artifactType = NLS::Core::Assets::ArtifactType::Material;
					else if (artifactTypeText == "Texture" || artifactTypeText == "texture")
						artifactType = NLS::Core::Assets::ArtifactType::Texture;
					else if (artifactTypeText == "Mesh" || artifactTypeText == "mesh")
						artifactType = NLS::Core::Assets::ArtifactType::Mesh;
					else if (artifactTypeText == "Model" || artifactTypeText == "model")
						artifactType = NLS::Core::Assets::ArtifactType::Model;
					imported = currentManifest && artifactType != NLS::Core::Assets::ArtifactType::Unknown;
					break;
				}
			}
		}
	}

	if (!CanStoreEditorAssetDragPayload(resourceFormatPath, meta->id, subAssetKey))
		return std::nullopt;

	const bool generatedModelPrefab =
		fileType == Utils::PathParser::EFileType::MODEL &&
		artifactType == NLS::Core::Assets::ArtifactType::Prefab;
	return MakeEditorAssetDragPayload(
		resourceFormatPath,
		meta->id,
		subAssetKey,
		artifactType,
		generatedModelPrefab,
		imported);
}

void RenameAsset(const std::string& p_prev, const std::string& p_new)
{
	std::filesystem::rename(p_prev, p_new);

	if (const std::string previousMetaPath = GetAssociatedMetaFile(p_prev); std::filesystem::exists(previousMetaPath))
	{
		if (const std::string newMetaPath = GetAssociatedMetaFile(p_new); !std::filesystem::exists(newMetaPath))
		{
			std::filesystem::rename(previousMetaPath, newMetaPath);
		}
		else
		{
			NLS_LOG_ERROR(newMetaPath + " is already existing, .meta creation failed");
		}
	}
}

void RemoveAsset(const std::string& p_toDelete)
{
	std::filesystem::remove(p_toDelete);

	if (const std::string metaPath = GetAssociatedMetaFile(p_toDelete); std::filesystem::exists(metaPath))
	{
		std::filesystem::remove(metaPath);
	}
}

class TexturePreview : public NLS::UI::IPlugin
{
public:
	TexturePreview() : image(nullptr, { 80, 80 })
	{

	}

	void SetPath(const std::string& p_path)
	{
        resourcePath = p_path;
	}

	virtual void Execute() override
	{
        if (NLS_SERVICE(NLS::UI::UIManager).IsItemHovered())
		{
            if (!texture && !resourcePath.empty())
            {
                texture = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>()[resourcePath];
                image.textureView = texture != nullptr
                    ? texture->GetOrCreateExplicitTextureView("AssetBrowser.Preview")
                    : nullptr;
            }
			NLS_SERVICE(NLS::UI::UIManager).BeginTooltip();
			image.Draw();
            NLS_SERVICE(NLS::UI::UIManager).EndTooltip();
		}
	}

	std::string resourcePath;
	Render::Resources::Texture2D* texture = nullptr;
	NLS::UI::Widgets::Image image;
};

class BrowserItemContextualMenu : public NLS::UI::ContextualMenu
{
public:
	BrowserItemContextualMenu(const std::string p_filePath, bool p_protected = false) : m_protected(p_protected), filePath(p_filePath) {}

	virtual void CreateList()
	{
		if (!m_protected)
		{
			auto& deleteAction = CreateWidget<MenuItem>("Delete");
			deleteAction.ClickedEvent += [this] { DeleteItem(); };

			auto& renameMenu = CreateWidget<MenuList>("Rename to...");

			auto& nameEditor = renameMenu.CreateWidget<InputText>("");
			nameEditor.selectAllOnClick = true;

			renameMenu.ClickedEvent += [this, &nameEditor]
			{
				nameEditor.content = Utils::PathParser::GetElementName(filePath);

				if (!std::filesystem::is_directory(filePath))
					if (size_t pos = nameEditor.content.rfind('.'); pos != std::string::npos)
						nameEditor.content = nameEditor.content.substr(0, pos);
			};

			nameEditor.EnterPressedEvent += [this](std::string p_newName)
			{
				if (!std::filesystem::is_directory(filePath))
					p_newName += '.' + Utils::PathParser::GetExtension(filePath);

				/* Clean the name (Remove special chars) */
				p_newName.erase(std::remove_if(p_newName.begin(), p_newName.end(), [](auto& c)
				{
					return std::find(FILENAMES_CHARS.begin(), FILENAMES_CHARS.end(), c) == FILENAMES_CHARS.end();
				}), p_newName.end());

				std::string containingFolderPath = Utils::PathParser::GetContainingFolder(filePath);
				std::string newPath = containingFolderPath + p_newName;
				std::string oldPath = filePath;

				if (filePath != newPath && !std::filesystem::exists(newPath))
					filePath = newPath;

				if (std::filesystem::is_directory(oldPath))
					filePath += '\\';

				RenamedEvent.Invoke(oldPath, newPath);
			};
		}
	}

	virtual void Execute() override
	{
		if (m_widgets.size() > 0)
			ContextualMenu::Execute();
	}

	virtual void DeleteItem() = 0;

public:
	bool m_protected;
	std::string filePath;
	Event<std::string> DestroyedEvent;
	Event<std::string, std::string> RenamedEvent;
};

class FolderContextualMenu : public BrowserItemContextualMenu
{
public:
	FolderContextualMenu(const std::string& p_filePath, bool p_protected = false) : BrowserItemContextualMenu(p_filePath, p_protected) {}

	virtual void CreateList() override
	{
		auto& showInExplorer = CreateWidget<MenuItem>("Show in explorer");
		showInExplorer.ClickedEvent += [this]
		{
            Platform::SystemCalls::ShowInExplorer(filePath);
		};

		if (!m_protected)
		{
			auto& importAssetHere = CreateWidget<MenuItem>("Import Here...");
			importAssetHere.ClickedEvent += [this]
			{
				if (EDITOR_EXEC(ImportAssetAtLocation(filePath)))
				{
					TreeNode* pluginOwner = reinterpret_cast<TreeNode*>(userData);
					pluginOwner->Close();
					EDITOR_EXEC(DelayAction(std::bind(&TreeNode::Open, pluginOwner)));
				}
			};

			auto& createMenu = CreateWidget<MenuList>("Create..");

			auto& createFolderMenu = createMenu.CreateWidget<MenuList>("Folder");
			auto& createSceneMenu = createMenu.CreateWidget<MenuList>("Scene");
			auto& createShaderMenu = createMenu.CreateWidget<MenuList>("Shader");
			auto& createMaterialMenu = createMenu.CreateWidget<MenuList>("Material");

			auto& createStandardShaderMenu = createShaderMenu.CreateWidget<MenuList>("Standard template");
			auto& createStandardPBRShaderMenu = createShaderMenu.CreateWidget<MenuList>("Standard PBR template");
			auto& createUnlitShaderMenu = createShaderMenu.CreateWidget<MenuList>("Unlit template");
			auto& createLambertShaderMenu = createShaderMenu.CreateWidget<MenuList>("Lambert template");

			auto& createEmptyMaterialMenu = createMaterialMenu.CreateWidget<MenuList>("Empty");
			auto& createStandardMaterialMenu = createMaterialMenu.CreateWidget<MenuList>("Standard");
			auto& createStandardPBRMaterialMenu = createMaterialMenu.CreateWidget<MenuList>("Standard PBR");
			auto& createUnlitMaterialMenu = createMaterialMenu.CreateWidget<MenuList>("Unlit");
			auto& createLambertMaterialMenu = createMaterialMenu.CreateWidget<MenuList>("Lambert");

			auto& createFolder = createFolderMenu.CreateWidget<InputText>("");
			auto& createScene = createSceneMenu.CreateWidget<InputText>("");

			auto& createEmptyMaterial = createEmptyMaterialMenu.CreateWidget<InputText>("");
			auto& createStandardMaterial = createStandardMaterialMenu.CreateWidget<InputText>("");
			auto& createStandardPBRMaterial = createStandardPBRMaterialMenu.CreateWidget<InputText>("");
			auto& createUnlitMaterial = createUnlitMaterialMenu.CreateWidget<InputText>("");
			auto& createLambertMaterial = createLambertMaterialMenu.CreateWidget<InputText>("");

			auto& createStandardShader = createStandardShaderMenu.CreateWidget<InputText>("");
			auto& createStandardPBRShader = createStandardPBRShaderMenu.CreateWidget<InputText>("");
			auto& createUnlitShader = createUnlitShaderMenu.CreateWidget<InputText>("");
			auto& createLambertShader = createLambertShaderMenu.CreateWidget<InputText>("");

			createFolderMenu.ClickedEvent += [&createFolder] { createFolder.content = ""; };
			createSceneMenu.ClickedEvent += [&createScene] { createScene.content = ""; };
			createStandardShaderMenu.ClickedEvent += [&createStandardShader] { createStandardShader.content = ""; };
			createStandardPBRShaderMenu.ClickedEvent += [&createStandardPBRShader] { createStandardPBRShader.content = ""; };
			createUnlitShaderMenu.ClickedEvent += [&createUnlitShader] { createUnlitShader.content = ""; };
			createLambertShaderMenu.ClickedEvent += [&createLambertShader] { createLambertShader.content = ""; };
			createEmptyMaterialMenu.ClickedEvent += [&createEmptyMaterial] { createEmptyMaterial.content = ""; };
			createStandardMaterialMenu.ClickedEvent += [&createStandardMaterial] { createStandardMaterial.content = ""; };
			createStandardPBRMaterialMenu.ClickedEvent += [&createStandardPBRMaterial] { createStandardPBRMaterial.content = ""; };
			createUnlitMaterialMenu.ClickedEvent += [&createUnlitMaterial] { createUnlitMaterial.content = ""; };
			createLambertMaterialMenu.ClickedEvent += [&createLambertMaterial] { createLambertMaterial.content = ""; };

			createFolder.EnterPressedEvent += [this](std::string newFolderName)
			{
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + (!fails ? newFolderName : newFolderName + " (" + std::to_string(fails) + ')');

					++fails;
				} while (std::filesystem::exists(finalPath));

				std::filesystem::create_directory(finalPath);

				ItemAddedEvent.Invoke(finalPath);
				Close();
			};

			createScene.EnterPressedEvent += [this](std::string newSceneName)
			{
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + (!fails ? newSceneName : newSceneName + " (" + std::to_string(fails) + ')') + ".scene";

					++fails;
				} while (std::filesystem::exists(finalPath));

				Engine::SceneSystem::Scene scene;
				if (!Engine::SceneSystem::SceneManager::SaveSceneToPath(scene, finalPath))
				{
					NLS_LOG_ERROR("Failed to create scene asset: " + finalPath);
					return;
				}

				ItemAddedEvent.Invoke(finalPath);
				Close();
			};

			createStandardShader.EnterPressedEvent += [this](std::string newShaderName)
			{
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + '\\' + (!fails ? newShaderName : newShaderName + " (" + std::to_string(fails) + ')') + ".hlsl";

					++fails;
				} while (std::filesystem::exists(finalPath));

				std::filesystem::copy_file(EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\Standard.hlsl", finalPath);
				ItemAddedEvent.Invoke(finalPath);
				Close();
			};

			createStandardPBRShader.EnterPressedEvent += [this](std::string newShaderName)
			{
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + Utils::PathParser::Separator() + (!fails ? newShaderName : newShaderName + " (" + std::to_string(fails) + ')') + ".hlsl";

					++fails;
				} while (std::filesystem::exists(finalPath));

				std::filesystem::copy_file(EDITOR_CONTEXT(engineAssetsPath) + "Shaders/Standard.hlsl", finalPath);
				ItemAddedEvent.Invoke(finalPath);
				Close();
			};

			createUnlitShader.EnterPressedEvent += [this](std::string newShaderName)
			{
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + '\\' + (!fails ? newShaderName : newShaderName + " (" + std::to_string(fails) + ')') + ".hlsl";

					++fails;
				} while (std::filesystem::exists(finalPath));

				std::filesystem::copy_file(EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\Unlit.hlsl", finalPath);
				ItemAddedEvent.Invoke(finalPath);
				Close();
			};

			createLambertShader.EnterPressedEvent += [this](std::string newShaderName)
			{
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + '\\' + (!fails ? newShaderName : newShaderName + " (" + std::to_string(fails) + ')') + ".hlsl";

					++fails;
				} while (std::filesystem::exists(finalPath));

				std::filesystem::copy_file(EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\Lambert.hlsl", finalPath);
				ItemAddedEvent.Invoke(finalPath);
				Close();
			};
			
			createEmptyMaterial.EnterPressedEvent += [this](std::string materialName)
			{
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + (!fails ? materialName : materialName + " (" + std::to_string(fails) + ')') + ".mat";

					++fails;
				} while (std::filesystem::exists(finalPath));

				{
					std::ofstream outfile(finalPath);
					outfile << "<root><shader>?</shader></root>" << std::endl; // Empty material content
				}

				ItemAddedEvent.Invoke(finalPath);

				if (auto instance = EDITOR_CONTEXT(materialManager)[EDITOR_EXEC(GetResourcePath(finalPath))])
				{
					auto& materialEditor = EDITOR_PANEL(NLS::Editor::Panels::MaterialEditor, "Material Editor");
					materialEditor.SetTarget(*instance);
					materialEditor.Open();
					materialEditor.Focus();
					materialEditor.Preview();
				}
				Close();
			};

			createStandardMaterial.EnterPressedEvent += [this](std::string materialName)
			{
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + (!fails ? materialName : materialName + " (" + std::to_string(fails) + ')') + ".mat";

					++fails;
				} while (std::filesystem::exists(finalPath));

				{
					std::ofstream outfile(finalPath);
					outfile << "<root><shader>:Shaders\\Standard.hlsl</shader></root>" << std::endl; // Empty standard material content
				}

				ItemAddedEvent.Invoke(finalPath);

				if (auto instance = EDITOR_CONTEXT(materialManager)[EDITOR_EXEC(GetResourcePath(finalPath))])
				{
					auto& materialEditor = EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor");
					materialEditor.SetTarget(*instance);
					materialEditor.Open();
					materialEditor.Focus();
					materialEditor.Preview();
				}
				Close();
			};

			createStandardPBRMaterial.EnterPressedEvent += [this](std::string materialName)
			{
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + (!fails ? materialName : materialName + " (" + std::to_string(fails) + ')') + ".mat";

					++fails;
				} while (std::filesystem::exists(finalPath));

				{
					std::ofstream outfile(finalPath);
					outfile << "<root><shader>:Shaders\\Standard.hlsl</shader></root>" << std::endl;
				}

				ItemAddedEvent.Invoke(finalPath);

				if (auto instance = EDITOR_CONTEXT(materialManager)[EDITOR_EXEC(GetResourcePath(finalPath))])
				{
					auto& materialEditor = EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor");
					materialEditor.SetTarget(*instance);
					materialEditor.Open();
					materialEditor.Focus();
					materialEditor.Preview();
				}
				Close();
			};

			createUnlitMaterial.EnterPressedEvent += [this](std::string materialName)
			{
				std::string newSceneName = "Material";
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + (!fails ? materialName : materialName + " (" + std::to_string(fails) + ')') + ".mat";

					++fails;
				} while (std::filesystem::exists(finalPath));

				{
					std::ofstream outfile(finalPath);
					outfile << "<root><shader>:Shaders\\Unlit.hlsl</shader></root>" << std::endl; // Empty unlit material content
				}

				ItemAddedEvent.Invoke(finalPath);

				if (auto instance = EDITOR_CONTEXT(materialManager)[EDITOR_EXEC(GetResourcePath(finalPath))])
				{
					auto& materialEditor = EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor");
					materialEditor.SetTarget(*instance);
					materialEditor.Open();
					materialEditor.Focus();
					materialEditor.Preview();
				}
				Close();
			};

			createLambertMaterial.EnterPressedEvent += [this](std::string materialName)
			{
				size_t fails = 0;
				std::string finalPath;

				do
				{
					finalPath = filePath + (!fails ? materialName : materialName + " (" + std::to_string(fails) + ')') + ".mat";

					++fails;
				} while (std::filesystem::exists(finalPath));

				{
					std::ofstream outfile(finalPath);
					outfile << "<root><shader>:Shaders\\Lambert.hlsl</shader></root>" << std::endl; // Empty unlit material content
				}

				ItemAddedEvent.Invoke(finalPath);

				if (auto instance = EDITOR_CONTEXT(materialManager)[EDITOR_EXEC(GetResourcePath(finalPath))])
				{
					auto& materialEditor = EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor");
					materialEditor.SetTarget(*instance);
					materialEditor.Open();
					materialEditor.Focus();
					materialEditor.Preview();
				}
				Close();
			};

			BrowserItemContextualMenu::CreateList();
		}
	}

	virtual void DeleteItem() override
	{
		using namespace NLS::Dialogs;
		MessageBox message("Delete folder", "Deleting a folder (and all its content) is irreversible, are you sure that you want to delete \"" + filePath + "\"?", MessageBox::EMessageType::WARNING, MessageBox::EButtonLayout::YES_NO);

		if (message.GetUserAction() == MessageBox::EUserAction::YES)
		{
			if (std::filesystem::exists(filePath) == true)
			{
				EDITOR_EXEC(PropagateFolderDestruction(filePath));
				std::filesystem::remove_all(filePath);
				DestroyedEvent.Invoke(filePath);
			}
		}
	}

public:
	Event<std::string> ItemAddedEvent;
};

class ScriptFolderContextualMenu : public FolderContextualMenu
{
public:
	ScriptFolderContextualMenu(const std::string& p_filePath, bool p_protected = false) : FolderContextualMenu(p_filePath, p_protected) {}

	void CreateScript(const std::string& p_name, const std::string& p_path)
	{
		std::string fileContent = "local " + p_name + " =\n{\n}\n\nfunction " + p_name + ":OnStart()\nend\n\nfunction " + p_name + ":OnUpdate(deltaTime)\nend\n\nreturn " + p_name;
		
		std::ofstream outfile(p_path);
		outfile << fileContent << std::endl; // Empty scene content

		ItemAddedEvent.Invoke(p_path);
		Close();
	}

	virtual void CreateList() override
	{
		FolderContextualMenu::CreateList();

		auto& newScriptMenu = CreateWidget<MenuList>("New script...");
		auto& nameEditor = newScriptMenu.CreateWidget<InputText>("");

		newScriptMenu.ClickedEvent += [this, &nameEditor]
		{
			nameEditor.content = Utils::PathParser::GetElementName("");
		};

		nameEditor.EnterPressedEvent += [this](std::string p_newName)
		{
			/* Clean the name (Remove special chars) */
			p_newName.erase(std::remove_if(p_newName.begin(), p_newName.end(), [](auto& c)
			{
				return std::find(FILENAMES_CHARS.begin(), FILENAMES_CHARS.end(), c) == FILENAMES_CHARS.end();
			}), p_newName.end());

			std::string newPath = filePath + p_newName + ".lua";

			if (!std::filesystem::exists(newPath))
			{
				CreateScript(p_newName, newPath);
			}
		};
	}
};

class FileContextualMenu : public BrowserItemContextualMenu
{
public:
	FileContextualMenu(const std::string& p_filePath, bool p_protected = false) : BrowserItemContextualMenu(p_filePath, p_protected) {}

	virtual void CreateList() override
	{
		auto& editAction = CreateWidget<MenuItem>("Open");

		editAction.ClickedEvent += [this]
		{
			Platform::SystemCalls::OpenFile(filePath);
		};

		if (!m_protected)
		{
			auto& duplicateAction = CreateWidget<MenuItem>("Duplicate");

			duplicateAction.ClickedEvent += [this]
			{
				std::string filePathWithoutExtension = filePath;

				if (size_t pos = filePathWithoutExtension.rfind('.'); pos != std::string::npos)
					filePathWithoutExtension = filePathWithoutExtension.substr(0, pos);

				std::string extension = "." + Utils::PathParser::GetExtension(filePath);

                auto filenameAvailable = [&extension](const std::string& target)
                {
                    return !std::filesystem::exists(target + extension);
                };

                const auto newNameWithoutExtension = Utils::String::GenerateUnique(filePathWithoutExtension, filenameAvailable);

				std::string finalPath = newNameWithoutExtension + extension;
				std::filesystem::copy(filePath, finalPath);

				DuplicateEvent.Invoke(finalPath);
			};
		}

		BrowserItemContextualMenu::CreateList();


        auto& editMetadata = CreateWidget<MenuItem>("Properties");

        editMetadata.ClickedEvent += [this]
        {
            auto& panel = EDITOR_PANEL(Editor::Panels::AssetProperties, "Asset Properties");
            std::string resourcePath = EDITOR_EXEC(GetResourcePath(filePath, m_protected));
            panel.SetTarget(resourcePath);
            panel.Open();
            panel.Focus();
        };
	}

	virtual void DeleteItem() override
	{
		using namespace NLS::Dialogs;
		MessageBox message("Delete file", "Deleting a file is irreversible, are you sure that you want to delete \"" + filePath + "\"?", MessageBox::EMessageType::WARNING, MessageBox::EButtonLayout::YES_NO);

		if (message.GetUserAction() == MessageBox::EUserAction::YES)
		{
			RemoveAsset(filePath);
			DestroyedEvent.Invoke(filePath);
			EDITOR_EXEC(PropagateFileRename(filePath, "?"));
		}
	}

public:
	Event<std::string> DuplicateEvent;
};

template<typename Resource, typename ResourceLoader>
class PreviewableContextualMenu : public FileContextualMenu
{
public:
	PreviewableContextualMenu(const std::string& p_filePath, bool p_protected = false) : FileContextualMenu(p_filePath, p_protected) {}

	virtual void CreateList() override
	{
		auto& previewAction = CreateWidget<MenuItem>("Preview");

		previewAction.ClickedEvent += [this]
		{
			Resource* resource = NLS::Core::ServiceLocator::Get<ResourceLoader>()[EDITOR_EXEC(GetResourcePath(filePath, m_protected))];
			auto& assetView = EDITOR_PANEL(Editor::Panels::AssetView, "Asset View");
			assetView.SetResource(resource);
			assetView.Open();
			assetView.Focus();
		};

		FileContextualMenu::CreateList();
	}
};

class ShaderContextualMenu : public FileContextualMenu
{
public:
	ShaderContextualMenu(const std::string& p_filePath, bool p_protected = false) : FileContextualMenu(p_filePath, p_protected) {}

	virtual void CreateList() override
	{
		FileContextualMenu::CreateList();

		auto& compileAction = CreateWidget<MenuItem>("Compile");

		compileAction.ClickedEvent += [this]
		{
            using namespace NLS::Core::ResourceManagement;
			auto& shaderManager = NLS_SERVICE(ShaderManager);
			std::string resourcePath = EDITOR_EXEC(GetResourcePath(filePath, m_protected));
			if (shaderManager.IsResourceRegistered(resourcePath))
			{
				/* Trying to recompile */
				Render::Resources::Loaders::ShaderLoader::Recompile(
                    *shaderManager[resourcePath],
                    filePath,
                    ShaderManager::ProjectAssetsRoot());
			}
			else
			{
				/* Trying to compile */
                Render::Resources::Shader* shader = NLS_SERVICE(ShaderManager)[resourcePath];
				if (shader)
					NLS_LOG_INFO("[COMPILE] \"" + filePath + "\": Success!");
			}
			
		};
	}
};

class ModelContextualMenu : public PreviewableContextualMenu<Render::Resources::Mesh, NLS::Core::ResourceManagement::MeshManager>
{
public:
	ModelContextualMenu(const std::string& p_filePath, bool p_protected = false) : PreviewableContextualMenu(p_filePath, p_protected) {}

	virtual void CreateList() override
	{
		auto& reimportAction = CreateWidget<MenuItem>("Reimport");

		reimportAction.ClickedEvent += [this]
		{
			if (m_protected)
				return;

			ReimportProjectAssetAsync(EDITOR_CONTEXT(projectAssetsPath), filePath);
		};

		PreviewableContextualMenu::CreateList();
	}
};

class TextureContextualMenu : public PreviewableContextualMenu<Render::Resources::Texture2D, NLS::Core::ResourceManagement::TextureManager>
{
public:
	TextureContextualMenu(const std::string& p_filePath, bool p_protected = false) : PreviewableContextualMenu(p_filePath, p_protected) {}

	virtual void CreateList() override
	{
		auto& reloadAction = CreateWidget<MenuItem>("Reload");

		reloadAction.ClickedEvent += [this]
		{
			auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
			std::string resourcePath = EDITOR_EXEC(GetResourcePath(filePath, m_protected));
			if (textureManager.IsResourceRegistered(resourcePath))
			{
				/* Trying to recompile */
				textureManager.AResourceManager::ReloadResource(resourcePath);
				EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor").Refresh();
			}
		};

		PreviewableContextualMenu::CreateList();
	}
};

class SceneContextualMenu : public FileContextualMenu
{
public:
	SceneContextualMenu(const std::string& p_filePath, bool p_protected = false) : FileContextualMenu(p_filePath, p_protected) {}

	virtual void CreateList() override
	{
		auto& editAction = CreateWidget<MenuItem>("Edit");

		editAction.ClickedEvent += [this]
		{
			EDITOR_EXEC(LoadSceneFromDisk(EDITOR_EXEC(GetResourcePath(filePath))));
		};

		FileContextualMenu::CreateList();
	}
};

class MaterialContextualMenu : public PreviewableContextualMenu<NLS::Render::Resources::Material, NLS::Core::ResourceManagement::MaterialManager>
{
public:
	MaterialContextualMenu(const std::string& p_filePath, bool p_protected = false) : PreviewableContextualMenu(p_filePath, p_protected) {}

	virtual void CreateList() override
	{
		auto& editAction = CreateWidget<MenuItem>("Edit");

		editAction.ClickedEvent += [this]
		{
            NLS::Render::Resources::Material* material = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager)[EDITOR_EXEC(GetResourcePath(filePath, m_protected))];
			if (material)
			{
				auto& materialEditor = EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor");
				materialEditor.SetTarget(*material);
				materialEditor.Open();
				materialEditor.Focus();
				
				NLS::Render::Resources::Material* resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>()[EDITOR_EXEC(GetResourcePath(filePath, m_protected))];
				auto& assetView = EDITOR_PANEL(Editor::Panels::AssetView, "Asset View");
				assetView.SetResource(resource);
				assetView.Open();
				assetView.Focus();
			}
		};

		auto& reload = CreateWidget<MenuItem>("Reload");
		reload.ClickedEvent += [this]
		{
			auto materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
			auto resourcePath = EDITOR_EXEC(GetResourcePath(filePath, m_protected));
            NLS::Render::Resources::Material* material = materialManager[resourcePath];
			if (material)
			{
				materialManager.AResourceManager::ReloadResource(resourcePath);
				EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor").Refresh();
			}
		};

		PreviewableContextualMenu::CreateList();
	}
};

Editor::Panels::AssetBrowser::AssetBrowser
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings,
	const std::string& p_engineAssetFolder,
	const std::string& p_projectAssetFolder,
	const std::string& p_projectScriptFolder
) :
	PanelWindow(p_title, p_opened, p_windowSettings),
	m_engineAssetFolder(p_engineAssetFolder),
	m_projectAssetFolder(p_projectAssetFolder)
{
	NLS::Editor::Assets::SetObjectReferencePickerAssetRoots(
		NLS::Editor::Assets::MakeProjectEditorAssetRoots(ProjectRootFromAssetsFolder(m_projectAssetFolder)));

	if (!std::filesystem::exists(m_projectAssetFolder))
	{
		std::filesystem::create_directories(m_projectAssetFolder);

		NLS::Dialogs::MessageBox message
		(
			"Assets folder not found",
			"The \"Assets/\" folders hasn't been found in your project directory.\nIt has been automatically generated",
            NLS::Dialogs::MessageBox::EMessageType::WARNING,
            NLS::Dialogs::MessageBox::EButtonLayout::OK
		);
	}

	auto& refreshButton = CreateWidget<Button>("Rescan assets");
	refreshButton.ClickedEvent += std::bind(&AssetBrowser::Refresh, this);
	refreshButton.lineBreak = false;
	refreshButton.idleBackgroundColor = { 0.18f, 0.19f, 0.21f };
	refreshButton.hoveredBackgroundColor = { 0.23f, 0.24f, 0.26f };
	refreshButton.clickedBackgroundColor = { 0.26f, 0.28f, 0.31f };

	auto& importButton = CreateWidget<Button>("Import asset");
	importButton.lineBreak = false;
	importButton.ClickedEvent += EDITOR_BIND(ImportAsset, m_projectAssetFolder);
	importButton.idleBackgroundColor = { 0.23f, 0.49f, 0.82f };
	importButton.hoveredBackgroundColor = { 0.29f, 0.58f, 0.93f };
	importButton.clickedBackgroundColor = { 0.18f, 0.41f, 0.71f };
	importButton.textColor = { 0.96f, 0.97f, 0.99f };

    CreateWidget<Spacing>(1);

	m_assetList = &CreateWidget<Group>();

	Fill();
}

void Editor::Panels::AssetBrowser::Fill()
{
	m_assetList->CreateWidget<Separator>();
	ConsiderItem(nullptr, std::filesystem::directory_entry(m_engineAssetFolder), true);
	m_assetList->CreateWidget<Separator>();
	ConsiderItem(nullptr, std::filesystem::directory_entry(m_projectAssetFolder), false);
}

void Editor::Panels::AssetBrowser::Clear()
{
	m_assetList->RemoveAllWidgets();
}

void Editor::Panels::AssetBrowser::Refresh()
{
	RefreshPreservingExpandedFolders();
}

void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()
{
	if (!m_watchersStartupQueued)
		StartWatchersAsync();

	CompleteWatcherStartupIfReady();
	if (m_startupWatcherPreimportGateOpen)
		ConsumeWatcherChangesAndSchedulePreimport();

	if (m_refreshRequested)
	{
		m_refreshRequested = false;
		RefreshPreservingExpandedFolders();
	}
}

void Editor::Panels::AssetBrowser::PrepareStartupWatchers()
{
	if (!m_watchersStartupQueued)
		StartWatchersSynchronously();

	if (m_watcherStartup.valid() &&
		m_watcherStartup.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
	{
		m_watcherStartup.wait();
	}

	CompleteWatcherStartupIfReady();
	ConsumeWatcherChangesAndSchedulePreimport();
	RequestRefresh();
}

void Editor::Panels::AssetBrowser::AdoptStartupWatchers(
	Core::AssetFileWatcher engineAssetsWatcher,
	Core::AssetFileWatcher projectAssetsWatcher)
{
	if (m_watcherStartup.valid())
		m_watcherStartup.wait();

	m_engineAssetsWatcher = std::move(engineAssetsWatcher);
	m_projectAssetsWatcher = std::move(projectAssetsWatcher);
	m_watchersStartupQueued = true;
	m_watchersReadyRefreshQueued = true;
	m_startupWatcherPreimportGateOpen = false;
	RequestRefresh();
}

bool Editor::Panels::AssetBrowser::RunStartupWatcherPreimport(
	const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink)
{
	using namespace NLS::Editor::Assets;

	CompleteWatcherStartupIfReady();
	bool allImported = true;
	for (;;)
	{
		const auto projectAssetChanges = m_projectAssetsWatcher.ConsumeChangedPaths();
		const auto engineAssetChanges = m_engineAssetsWatcher.ConsumeChangedPaths();
		if (!engineAssetChanges.empty())
			RequestRefresh();
		if (projectAssetChanges.empty())
			return allImported;

		std::vector<std::filesystem::path> relativeChanges;
		relativeChanges.reserve(projectAssetChanges.size());
		const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
		for (const auto& changedPath : projectAssetChanges)
		{
			auto relative = changedPath.lexically_relative(projectRoot);
			if (relative.empty() || relative.is_absolute())
				relative = changedPath;
			relativeChanges.push_back(relative.lexically_normal());
		}

		AssetDatabaseFacade database(MakeProjectEditorAssetRoots(projectRoot));
		ImportProgressTracker tracker;
		if (progressSink)
			tracker.Subscribe(progressSink);
		AssetPreimportScheduler preimportScheduler;
		const auto imported = preimportScheduler.Run(
			database,
			tracker,
			{AssetPreimportReason::FileWatcherChanged, std::move(relativeChanges)});
		allImported = allImported && imported;
	}
}

bool Editor::Panels::AssetBrowser::CompleteStartupWatcherPreimportGate(
	const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink)
{
	const auto imported = RunStartupWatcherPreimport(progressSink);
	if (!imported)
		return false;

	m_startupWatcherPreimportGateOpen = true;
	RequestRefresh();
	return true;
}

void Editor::Panels::AssetBrowser::CompleteWatcherStartupIfReady()
{
	if (m_watcherStartup.valid() &&
		m_watcherStartup.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		m_watcherStartup.get();
		if (!m_watchersReadyRefreshQueued)
		{
			m_watchersReadyRefreshQueued = true;
			RequestRefresh();
		}
	}
}

void Editor::Panels::AssetBrowser::ConsumeWatcherChangesAndSchedulePreimport()
{
	if (!m_startupWatcherPreimportGateOpen)
		return;

	const auto engineAssetChanges = m_engineAssetsWatcher.ConsumeChangedPaths();
	const auto projectAssetChanges = m_projectAssetsWatcher.ConsumeChangedPaths();
	const bool engineAssetsChanged = !engineAssetChanges.empty();
	const bool projectAssetsChanged = !projectAssetChanges.empty();
	if (projectAssetsChanged)
	{
		std::vector<std::filesystem::path> relativeChanges;
		relativeChanges.reserve(projectAssetChanges.size());
		const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
		for (const auto& changedPath : projectAssetChanges)
		{
			auto relative = changedPath.lexically_relative(projectRoot);
			if (relative.empty() || relative.is_absolute())
				relative = changedPath;
			relativeChanges.push_back(relative.lexically_normal());
		}
		ScheduleProjectAssetPreimport({
			NLS::Editor::Assets::AssetPreimportReason::FileWatcherChanged,
			std::move(relativeChanges)
		});
	}
	if (engineAssetsChanged || projectAssetsChanged)
		RequestRefresh();
}

void Editor::Panels::AssetBrowser::RequestRefresh()
{
	m_refreshRequested = true;
}

void Editor::Panels::AssetBrowser::ScheduleProjectAssetPreimport(
	NLS::Editor::Assets::AssetPreimportRequest request)
{
	using namespace NLS::Editor::Assets;

	AssetPreimportScheduler scheduler;
	if (!scheduler.ShouldRunForReason(request.reason))
		return;

	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	if (projectRoot.empty())
	{
		NLS_LOG_ERROR("Failed to resolve project root for asset preimport.");
		return;
	}

	if (m_projectAssetPreimportRunning)
	{
		m_pendingProjectAssetPreimportRequest = m_pendingProjectAssetPreimportRequest.has_value()
			? MergeAssetPreimportRequests(std::move(*m_pendingProjectAssetPreimportRequest), request)
			: std::move(request);
		return;
	}
	m_projectAssetPreimportRunning = true;

	auto& tracker = EDITOR_CONTEXT(importProgressTracker);
	EDITOR_EXEC(TrackBackgroundTask([projectRoot, request = std::move(request), &tracker]
	{
		AssetDatabaseFacade database(MakeProjectEditorAssetRoots(projectRoot));
		AssetPreimportScheduler preimportScheduler;
		const auto imported = preimportScheduler.Run(database, tracker, request);
		EDITOR_EXEC(DelayAction([reason = request.reason, imported]
		{
			auto& assetBrowser = EDITOR_PANEL(NLS::Editor::Panels::AssetBrowser, "Asset Browser");
			assetBrowser.m_projectAssetPreimportRunning = false;
			if (assetBrowser.m_pendingProjectAssetPreimportRequest.has_value())
			{
				auto pendingRequest = std::move(*assetBrowser.m_pendingProjectAssetPreimportRequest);
				assetBrowser.m_pendingProjectAssetPreimportRequest.reset();
				assetBrowser.ScheduleProjectAssetPreimport(std::move(pendingRequest));
			}
			assetBrowser.RequestRefresh();
			if (imported)
			{
				NLS_LOG_INFO(std::string("Asset preimport completed after ") + AssetPreimportReasonLabel(reason));
			}
			else
			{
				NLS_LOG_ERROR(std::string("Asset preimport failed after ") + AssetPreimportReasonLabel(reason));
			}
		}));
	}));
}

void Editor::Panels::AssetBrowser::RefreshPreservingExpandedFolders()
{
	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	if (!projectRoot.empty())
	{
		NLS::Editor::Assets::AssetDatabaseFacade database(
			NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
		NLS::Editor::Assets::SetObjectReferencePickerEntries(
			database.Refresh()
				? NLS::Editor::Assets::BuildObjectReferencePickerEntries(database)
				: std::vector<NLS::Editor::Assets::ObjectReferencePickerEntry> {});
	}
	else
	{
		NLS::Editor::Assets::SetObjectReferencePickerEntries({});
	}

	Clear();
	Fill();
}

void Editor::Panels::AssetBrowser::ParseFolder(TreeNode& p_root, const std::filesystem::directory_entry& p_directory, bool p_isEngineItem, bool p_scriptFolder)
{
	/* Iterates another time to display list files */
	for (auto& item : std::filesystem::directory_iterator(p_directory))
		if (item.is_directory())
			ConsiderItem(&p_root, item, p_isEngineItem, false, p_scriptFolder);

	/* Iterates another time to display list files */
	for (auto& item : std::filesystem::directory_iterator(p_directory))
		if (!item.is_directory())
			ConsiderItem(&p_root, item, p_isEngineItem, false, p_scriptFolder);
}

void Editor::Panels::AssetBrowser::ConsiderItem(TreeNode* p_root, const std::filesystem::directory_entry& p_entry, bool p_isEngineItem, bool p_autoOpen, bool p_scriptFolder)
{
	bool isDirectory = p_entry.is_directory();
	std::string path = p_entry.path().string();
	while (!path.empty() && (path.back() == '\\' || path.back() == '/'))
		path.pop_back();
	std::string itemname = Utils::PathParser::GetElementName(path);
	if (isDirectory && path.back() != '\\') // Add '\\' if is directory and backslash is missing
		path += '\\';
	std::string resourceFormatPath = EDITOR_EXEC(GetResourcePath(path, p_isEngineItem));
	bool protectedItem = !p_root || p_isEngineItem;

	Utils::PathParser::EFileType fileType = Utils::PathParser::GetFileType(itemname);

	// Unknown file, so we skip it
	if (fileType == Utils::PathParser::EFileType::UNKNOWN && !isDirectory)
	{
		return;
	}

	/* If there is a given treenode (p_root) we attach the new widget to it */
	auto& itemGroup = p_root ? p_root->CreateWidget<Group>() : m_assetList->CreateWidget<Group>();

	/* Find the icon to apply to the item */
    auto* iconTexture = isDirectory
        ? EDITOR_CONTEXT(editorResources)->GetTexture("Icon_Folder")
        : EDITOR_CONTEXT(editorResources)->GetFileIcon(itemname);

	itemGroup.CreateWidget<UI::Widgets::Image>(
        iconTexture != nullptr ? iconTexture->GetOrCreateExplicitTextureView("AssetBrowser.ItemIcon") : nullptr,
        Maths::Vector2{ 16, 16 }).lineBreak = false;

	/* If the entry is a directory, the content must be a tree node, otherwise (= is a file), a text will suffice */
	if (isDirectory)
	{
		auto& treeNode = itemGroup.CreateWidget<TreeNode>(itemname);

		if (p_autoOpen || m_expandedFolders.contains(path))
			treeNode.Open();

		auto& ddSource = treeNode.AddPlugin<UI::DDSource<std::pair<std::string, Group*>>>("Folder", resourceFormatPath, std::make_pair(resourceFormatPath, &itemGroup));
		
		if (!p_root || p_scriptFolder)
			treeNode.RemoveAllPlugins();

		auto& contextMenu = !p_scriptFolder ? treeNode.AddPlugin<FolderContextualMenu>(path, protectedItem && resourceFormatPath != "") : treeNode.AddPlugin<ScriptFolderContextualMenu>(path, protectedItem && resourceFormatPath != "");
		contextMenu.userData = static_cast<void*>(&treeNode);

		contextMenu.ItemAddedEvent += [this, &treeNode, path, p_isEngineItem, p_scriptFolder] (std::string p_string)
		{
			treeNode.Open();
			treeNode.RemoveAllWidgets();
			ParseFolder(treeNode, std::filesystem::directory_entry(Utils::PathParser::GetContainingFolder(p_string)), p_isEngineItem, p_scriptFolder);
		};

		if (!p_scriptFolder)
		{
			if (!p_isEngineItem) /* Prevent engine item from being DDTarget (Can't Drag and drop to engine folder) */
			{
				treeNode.AddPlugin<UI::DDTarget<std::pair<std::string, Group*>>>("Folder").DataReceivedEvent += [this, &treeNode, path, p_isEngineItem](std::pair<std::string, Group*> p_data)
				{
					if (!p_data.first.empty())
					{
						std::string folderReceivedPath = EDITOR_EXEC(GetRealPath(p_data.first));

						std::string folderName = Utils::PathParser::GetElementName(folderReceivedPath) + '\\';
						std::string prevPath = folderReceivedPath;
						std::string correctPath = m_pathUpdate.find(&treeNode) != m_pathUpdate.end() ? m_pathUpdate.at(&treeNode) : path;
						std::string newPath = correctPath + folderName;

						if (!(newPath.find(prevPath) != std::string::npos) || prevPath == newPath)
						{
							if (!std::filesystem::exists(newPath))
							{
								bool isEngineFolder = p_data.first.at(0) == ':';

								if (isEngineFolder) /* Copy dd folder from Engine resources */
									std::filesystem::copy(prevPath, newPath, std::filesystem::copy_options::recursive);
								else
								{
									RenameAsset(prevPath, newPath);
									EDITOR_EXEC(PropagateFolderRename(prevPath, newPath));
								}

								treeNode.Open();
								treeNode.RemoveAllWidgets();
								ParseFolder(treeNode, std::filesystem::directory_entry(correctPath), p_isEngineItem);

								if (!isEngineFolder)
									p_data.second->Destroy();
							}
							else if (prevPath == newPath)
							{
								// Ignore
							}
							else
							{
								using namespace NLS::Dialogs;

								MessageBox errorMessage("Folder already exists", "You can't move this folder to this location because the name is already taken", MessageBox::EMessageType::ERROR, MessageBox::EButtonLayout::OK);
							}
						}
						else
						{
							using namespace NLS::Dialogs;

							MessageBox errorMessage("Wow!", "Crazy boy!", MessageBox::EMessageType::ERROR, MessageBox::EButtonLayout::OK);
						}
					}
				};

				auto moveOrCopyFileIntoFolder = [this, &treeNode, path, p_isEngineItem](
					const std::string& receivedResourcePath,
					UI::Widgets::Group* receivedGroup)
				{
					if (!receivedResourcePath.empty())
					{
						std::string fileReceivedPath = EDITOR_EXEC(GetRealPath(receivedResourcePath));

						std::string fileName = Utils::PathParser::GetElementName(fileReceivedPath);
						std::string prevPath = fileReceivedPath;
						std::string correctPath = m_pathUpdate.find(&treeNode) != m_pathUpdate.end() ? m_pathUpdate.at(&treeNode) : path;
						std::string newPath = correctPath + fileName;

						if (!std::filesystem::exists(newPath))
						{
							bool isEngineFile = receivedResourcePath.at(0) == ':';

							if (isEngineFile) /* Copy dd file from Engine resources */
								std::filesystem::copy_file(prevPath, newPath);
							else
							{
								RenameAsset(prevPath, newPath);
								EDITOR_EXEC(PropagateFileRename(prevPath, newPath));
							}

							treeNode.Open();
							treeNode.RemoveAllWidgets();
							ParseFolder(treeNode, std::filesystem::directory_entry(correctPath), p_isEngineItem);

							if (!isEngineFile && receivedGroup)
								receivedGroup->Destroy();

							if (!p_isEngineItem)
								ScheduleProjectAssetPreimport({
									NLS::Editor::Assets::AssetPreimportReason::AssetCopiedOrMoved,
									{EditorAssetPathFromAbsolutePath(m_projectAssetFolder, newPath)}
								});
						}
						else if (prevPath == newPath)
						{
							// Ignore
						}
						else
						{
							using namespace NLS::Dialogs;

							MessageBox errorMessage("File already exists", "You can't move this file to this location because the name is already taken", MessageBox::EMessageType::ERROR, MessageBox::EButtonLayout::OK);
						}
					}
				};

				treeNode.AddPlugin<UI::DDTarget<std::pair<std::string, Group*>>>("File").DataReceivedEvent += [moveOrCopyFileIntoFolder](std::pair<std::string, Group*> p_data)
				{
					moveOrCopyFileIntoFolder(p_data.first, p_data.second);
				};

				treeNode.AddPlugin<UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>(
					NLS::Editor::Assets::kEditorAssetDragPayloadType).DataReceivedEvent += [moveOrCopyFileIntoFolder](NLS::Editor::Assets::EditorAssetDragPayload p_data)
				{
					moveOrCopyFileIntoFolder(NLS::Editor::Assets::GetEditorAssetDragPayloadPath(p_data), nullptr);
				};

				treeNode.AddPlugin<UI::DDTarget<std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>>>("GameObject").DataReceivedEvent += [this, &treeNode, path, p_isEngineItem](std::pair<Engine::GameObject*, UI::Widgets::TreeNode*> p_data)
				{
					if (!p_data.first || p_isEngineItem)
						return;

					const std::string correctPath = m_pathUpdate.find(&treeNode) != m_pathUpdate.end() ? m_pathUpdate.at(&treeNode) : path;
					const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
					const auto destinationFolder = EditorAssetFolderFromAbsolutePath(m_projectAssetFolder, correctPath);
					if (projectRoot.empty() || destinationFolder.empty())
					{
						NLS_LOG_ERROR("Failed to resolve prefab destination folder for hierarchy drop: " + correctPath);
						return;
					}

					NLS::Editor::Assets::AssetDatabaseFacade database(
						NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
					if (!database.Refresh())
					{
						NLS_LOG_ERROR("Failed to refresh asset database before saving prefab from hierarchy drop.");
						return;
					}

					const auto result = NLS::Editor::Assets::AssetDragDropWorkflow().Execute({
						{NLS::Editor::Assets::DragPayloadKind::HierarchyObject, {}, {}, nullptr, p_data.first},
						{NLS::Editor::Assets::DropTargetKind::AssetBrowserFolder, nullptr, nullptr, 0u, false, destinationFolder},
						{},
						NLS::Editor::Assets::DragDropOperationKind::SaveAsPrefab,
						&database
					});

					if (result.status != NLS::Editor::Assets::DragDropOperationStatus::Committed)
					{
						for (const auto& diagnostic : result.diagnostics)
							NLS_LOG_ERROR(diagnostic.code + ": " + diagnostic.message);
						return;
					}

					treeNode.Open();
					treeNode.RemoveAllWidgets();
					ParseFolder(treeNode, std::filesystem::directory_entry(correctPath), p_isEngineItem);
				};
			}

			contextMenu.DestroyedEvent += [&itemGroup](std::string p_deletedPath) { itemGroup.Destroy(); };

			contextMenu.RenamedEvent += [this, &treeNode, path, &ddSource, p_isEngineItem](std::string p_prev, std::string p_newPath)
			{
				p_newPath += '\\';

				if (!std::filesystem::exists(p_newPath)) // Do not rename a folder if it already exists
				{
					RenameAsset(p_prev, p_newPath);
					EDITOR_EXEC(PropagateFolderRename(p_prev, p_newPath));
					std::string elementName = Utils::PathParser::GetElementName(p_newPath);
					std::string data = Utils::PathParser::GetContainingFolder(ddSource.data.first) + elementName + "\\";
					ddSource.data.first = data;
					ddSource.tooltip = data;
					treeNode.name = elementName;
					treeNode.Open();
					treeNode.RemoveAllWidgets();
					ParseFolder(treeNode, std::filesystem::directory_entry(p_newPath), p_isEngineItem);
					m_pathUpdate[&treeNode] = p_newPath;
				}
				else
				{
					using namespace NLS::Dialogs;

					MessageBox errorMessage("Folder already exists", "You can't rename this folder because the given name is already taken", MessageBox::EMessageType::ERROR, MessageBox::EButtonLayout::OK);
				}
			};

			contextMenu.ItemAddedEvent += [this, &treeNode, p_isEngineItem](std::string p_path)
			{
				treeNode.RemoveAllWidgets();
				ParseFolder(treeNode, std::filesystem::directory_entry(Utils::PathParser::GetContainingFolder(p_path)), p_isEngineItem);
			};

		}
		
		contextMenu.CreateList();

		treeNode.OpenedEvent += [this, &treeNode, path, p_isEngineItem, p_scriptFolder]
		{
			m_expandedFolders.insert(path);
			treeNode.RemoveAllWidgets();
			ParseFolder(treeNode, std::filesystem::directory_entry(path), p_isEngineItem, p_scriptFolder);
		};

		treeNode.ClosedEvent += [this, &treeNode, path]
		{
			m_expandedFolders.erase(path);
			treeNode.RemoveAllWidgets();
		};
	}
	else
	{
		auto& clickableText = itemGroup.CreateWidget<TextClickable>(itemname);

		FileContextualMenu* contextMenu = nullptr;

		switch (fileType)
		{
		case Utils::PathParser::EFileType::MODEL:		contextMenu = &clickableText.AddPlugin<ModelContextualMenu>(path, protectedItem);		break;
		case Utils::PathParser::EFileType::TEXTURE:	contextMenu = &clickableText.AddPlugin<TextureContextualMenu>(path, protectedItem); 	break;
		case Utils::PathParser::EFileType::SHADER:		contextMenu = &clickableText.AddPlugin<ShaderContextualMenu>(path, protectedItem);		break;
		case Utils::PathParser::EFileType::MATERIAL:	contextMenu = &clickableText.AddPlugin<MaterialContextualMenu>(path, protectedItem);	break;
		case Utils::PathParser::EFileType::SCENE:		contextMenu = &clickableText.AddPlugin<SceneContextualMenu>(path, protectedItem);		break;
		default: contextMenu = &clickableText.AddPlugin<FileContextualMenu>(path, protectedItem); break;
		}

		contextMenu->CreateList();

		contextMenu->DestroyedEvent += [&itemGroup](std::string p_deletedPath)
		{
			itemGroup.Destroy();

			if (EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath() == p_deletedPath) // Modify current scene source path if the renamed file is the current scene
				EDITOR_CONTEXT(sceneManager).ForgetCurrentSceneSourcePath();
		};

		const auto assetPayload = p_isEngineItem
			? std::optional<NLS::Editor::Assets::EditorAssetDragPayload> {}
			: BuildEditorAssetDragPayloadForFile(m_projectAssetFolder, path, resourceFormatPath, fileType);
		auto& ddSource = clickableText.AddPlugin<UI::DDSource<std::pair<std::string, Group*>>>
		(
			"File",
			resourceFormatPath,
			std::make_pair(resourceFormatPath, &itemGroup)
		);
		if (assetPayload)
		{
			clickableText.AddPlugin<UI::DDSource<NLS::Editor::Assets::EditorAssetDragPayload>>(
				NLS::Editor::Assets::kEditorAssetDragPayloadType,
				resourceFormatPath,
				*assetPayload);
		}

		if (!p_isEngineItem && fileType == Utils::PathParser::EFileType::MODEL)
		{
			NLS::Editor::Assets::AssetDatabaseFacade database(
				NLS::Editor::Assets::MakeProjectEditorAssetRoots(ProjectRootFromAssetsFolder(m_projectAssetFolder)));
			if (database.Refresh())
			{
				for (const auto& subAsset : NLS::Editor::Assets::BuildAssetBrowserSubAssetEntries(
					database,
					resourceFormatPath))
				{
					if (!NLS::Editor::Assets::CanStoreEditorAssetDragPayload(
						subAsset.dragResourcePath,
						subAsset.assetId,
						subAsset.subAssetKey))
					{
						continue;
					}

					auto& subAssetText = itemGroup.CreateWidget<TextClickable>("  " + subAsset.displayName);
					subAssetText.AddPlugin<UI::DDSource<NLS::Editor::Assets::EditorAssetDragPayload>>(
						NLS::Editor::Assets::kEditorAssetDragPayloadType,
						subAsset.dragResourcePath,
						NLS::Editor::Assets::MakeEditorAssetDragPayload(
							subAsset.dragResourcePath,
							subAsset.assetId,
							subAsset.subAssetKey,
							subAsset.artifactType,
							false,
							true));
				}
			}
		}

		clickableText.ClickedEvent += [this, &clickableText, resourceFormatPath]
		{
			if (m_selectedAsset && m_selectedAsset != &clickableText)
				m_selectedAsset->selected = false;
			m_selectedAsset = &clickableText;
			clickableText.selected = true;

			auto& assetProperties = EDITOR_PANEL(Editor::Panels::AssetProperties, "Asset Properties");
			assetProperties.SetTarget(resourceFormatPath);
			assetProperties.Open();
			assetProperties.Focus();
		};

		contextMenu->RenamedEvent += [&ddSource, &clickableText, p_scriptFolder](std::string p_prev, std::string p_newPath)
		{
			if (p_newPath != p_prev)
			{
				if (!std::filesystem::exists(p_newPath))
				{
					RenameAsset(p_prev, p_newPath);
					std::string elementName = Utils::PathParser::GetElementName(p_newPath);
					ddSource.data.first = Utils::PathParser::GetContainingFolder(ddSource.data.first) + elementName;

					if (!p_scriptFolder)
					{
						EDITOR_EXEC(PropagateFileRename(p_prev, p_newPath));
						if (EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath() == p_prev) // Modify current scene source path if the renamed file is the current scene
							EDITOR_CONTEXT(sceneManager).StoreCurrentSceneSourcePath(p_newPath);
					}
					else
					{
						EDITOR_EXEC(PropagateScriptRename(p_prev, p_newPath));
					}

					clickableText.content = elementName;
				}
				else
				{
					using namespace NLS::Dialogs;

					MessageBox errorMessage("File already exists", "You can't rename this file because the given name is already taken", MessageBox::EMessageType::ERROR, MessageBox::EButtonLayout::OK);
				}
			}
		};

		contextMenu->DuplicateEvent += [this, &clickableText, p_root, path, p_isEngineItem] (std::string newItem)
		{
			EDITOR_EXEC(DelayAction(std::bind(&AssetBrowser::ConsiderItem, this, p_root, std::filesystem::directory_entry{ newItem }, p_isEngineItem, false, false), 0));
		};

		if (fileType == Utils::PathParser::EFileType::TEXTURE)
		{
			auto& texturePreview = clickableText.AddPlugin<TexturePreview>();
			texturePreview.SetPath(resourceFormatPath);
		}

		if (fileType == Utils::PathParser::EFileType::PREFAB)
		{
			clickableText.DoubleClickedEvent += [this, resourceFormatPath]
			{
				const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
				NLS::Editor::Assets::AssetDatabaseFacade database(
					NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
				if (!database.Refresh())
				{
					NLS_LOG_ERROR("Failed to refresh asset database before opening prefab: " + resourceFormatPath);
					return;
				}

				const auto prefabSubAssetKey = "prefab:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
				auto prefab = database.LoadPrefabArtifactAtPath(resourceFormatPath, prefabSubAssetKey);
				if (!prefab.has_value())
				{
					NLS_LOG_ERROR("Failed to load prefab artifact for prefab stage: " + resourceFormatPath);
					return;
				}

				auto stage = NLS::Editor::Assets::PrefabUtilityFacade().LoadPrefabContents({
					&*prefab,
					prefab->assetId,
					prefabSubAssetKey,
					prefab->generatedModelPrefab,
					resourceFormatPath
				});
				if (stage.status != NLS::Editor::Assets::PrefabOperationStatus::Committed)
				{
					for (const auto& diagnostic : stage.diagnostics)
						NLS_LOG_ERROR(diagnostic.code + ": " + diagnostic.message);
					return;
				}

				EDITOR_EXEC(GetContext()).activePrefabStage = std::move(stage.stage);
				EDITOR_EXEC(NotifyPrefabStageOpened());
				EDITOR_PANEL(NLS::Editor::Panels::Hierarchy, "Hierarchy").RebuildFromCurrentScene();
				EDITOR_PANEL(NLS::Editor::Panels::SceneView, "Scene View").Focus();
				NLS_LOG_INFO("Opened prefab stage: " + resourceFormatPath);
			};
		}
		else if (fileType == Utils::PathParser::EFileType::MODEL ||
			fileType == Utils::PathParser::EFileType::TEXTURE ||
			fileType == Utils::PathParser::EFileType::MATERIAL)
		{
			clickableText.DoubleClickedEvent += [resourceFormatPath]
			{
				auto& assetProperties = EDITOR_PANEL(Editor::Panels::AssetProperties, "Asset Properties");
				assetProperties.SetTarget(resourceFormatPath);

				auto& assetView = EDITOR_PANEL(Editor::Panels::AssetView, "Asset View");
				assetProperties.Preview();
				assetView.Open();
				assetView.Focus();
			};
		}
		else if (fileType == Utils::PathParser::EFileType::SCENE)
		{
			clickableText.DoubleClickedEvent += [path]
			{
				EDITOR_EXEC(LoadSceneFromDisk(EDITOR_EXEC(GetResourcePath(path))));
			};
		}

	}
}

void Editor::Panels::AssetBrowser::StartWatchersAsync()
{
	m_watchersStartupQueued = true;
	m_watcherStartup = std::async(
		std::launch::async,
		[this]
		{
			const auto engineWatcherStarted = m_engineAssetsWatcher.Start(m_engineAssetFolder);
			const auto projectWatcherStarted = m_projectAssetsWatcher.Start(m_projectAssetFolder);
			auto report = NLS::Editor::Assets::BuildAssetWatcherStartupReport(
				m_engineAssetFolder,
				engineWatcherStarted,
				m_projectAssetFolder,
				projectWatcherStarted);
			for (const auto& diagnostic : report.diagnostics)
				NLS_LOG_WARNING(diagnostic.message);
		});
}

void Editor::Panels::AssetBrowser::StartWatchersSynchronously()
{
	m_watchersStartupQueued = true;
	const auto engineWatcherStarted = m_engineAssetsWatcher.Start(m_engineAssetFolder);
	const auto projectWatcherStarted = m_projectAssetsWatcher.Start(m_projectAssetFolder);
	auto report = NLS::Editor::Assets::BuildAssetWatcherStartupReport(
		m_engineAssetFolder,
		engineWatcherStarted,
		m_projectAssetFolder,
		projectWatcherStarted);
	for (const auto& diagnostic : report.diagnostics)
		NLS_LOG_WARNING(diagnostic.message);
}
