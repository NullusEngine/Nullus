#include <array>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <exception>
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
#include <UI/Plugins/DragDrop.h>
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
#include <Image.h>

#include "Panels/MaterialEditor.h"
#include "Panels/AssetBrowser.h"
#include "Panels/AssetView.h"
#include "Panels/AssetProperties.h"
#include "Panels/Hierarchy.h"
#include "Panels/SceneView.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetManifestJson.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/EditorAssetPath.h"
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
#include "Rendering/Resources/Texture2D.h"

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

std::string NormalizeProjectBrowserPath(std::string path)
{
	return NLS::Editor::Assets::NormalizeAssetBrowserProjectRelativePath(std::move(path));
}

std::string NormalizeProjectBrowserPath(const std::filesystem::path& path)
{
	const auto text = path.lexically_normal().generic_u8string();
	return NLS::Editor::Assets::NormalizeAssetBrowserProjectRelativePath(
		{ reinterpret_cast<const char*>(text.data()), text.size() });
}

void InvalidateAssetThumbnailMetadataForImagePath(const std::string& normalizedImagePath)
{
	auto metadataPath = std::filesystem::path(normalizedImagePath);
	metadataPath.replace_extension(".json");
	std::error_code error;
	std::filesystem::remove(metadataPath, error);
}

bool IsProjectBrowserAncestorOf(
	const std::string& ancestor,
	const std::string& descendant)
{
	const auto normalizedAncestor = NormalizeProjectBrowserPath(ancestor);
	const auto normalizedDescendant = NormalizeProjectBrowserPath(descendant);
	return normalizedDescendant == normalizedAncestor ||
		(normalizedDescendant.size() > normalizedAncestor.size() &&
		 normalizedDescendant.compare(0u, normalizedAncestor.size(), normalizedAncestor) == 0 &&
		 normalizedDescendant[normalizedAncestor.size()] == '/');
}

void AddProjectBrowserAncestorFolders(
	std::unordered_set<std::string>& expandedFolders,
	const std::string& projectRelativePath)
{
	const auto normalized = NormalizeProjectBrowserPath(projectRelativePath);
	std::string current;
	size_t offset = 0u;
	while (offset <= normalized.size())
	{
		const auto separator = normalized.find('/', offset);
		const auto end = separator == std::string::npos ? normalized.size() : separator;
		const auto length = end - offset;
		if (length > 0u)
		{
			if (!current.empty())
				current += '/';
			current.append(normalized, offset, length);
			const auto currentText = NormalizeProjectBrowserPath(current);
			if (currentText != normalized)
				expandedFolders.insert(currentText);
		}

		if (separator == std::string::npos)
			break;
		offset = separator + 1u;
	}
}

ImU32 AssetBrowserItemColor(const NLS::Editor::Assets::AssetBrowserItemType type)
{
	const auto color = NLS::Editor::Assets::AssetBrowserItemTypeDisplayColor(type);
	return IM_COL32(color.red, color.green, color.blue, color.alpha);
}

std::string EllipsizeAssetBrowserLabel(
	const std::string& text,
	const float maxWidth)
{
	if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
		return text;

	const std::string ellipsis = "...";
	std::string fitted = text;
	while (!fitted.empty())
	{
		fitted.pop_back();
		const auto candidate = fitted + ellipsis;
		if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth)
			return candidate;
	}
	return ellipsis;
}

constexpr size_t kMaxResidentAssetBrowserThumbnailTextures = 256u;
constexpr size_t kMaxAssetBrowserThumbnailTextureLoadsPerFrame = 4u;
constexpr size_t kMaxAssetBrowserThumbnailTextureDecodesInFlight = 4u;
constexpr uint32_t kMaxAssetBrowserCachedThumbnailTextureDimension = 512u;
constexpr ImVec2 kAssetBrowserImageUv0(0.0f, 1.0f);
constexpr ImVec2 kAssetBrowserImageUv1(1.0f, 0.0f);

uint32_t AssetBrowserThumbnailRequestSize(const float thumbnailSize)
{
	return static_cast<uint32_t>((std::max)(1.0f, std::round(thumbnailSize)));
}

std::string EnsureTrailingPathSeparator(std::filesystem::path path)
{
	auto text = path.lexically_normal().string();
	if (!text.empty() && text.back() != '\\' && text.back() != '/')
		text += Utils::PathParser::Separator();
	return text;
}

std::string ProjectBrowserLegacyResourcePath(std::string projectRelativePath)
{
	projectRelativePath = NormalizeProjectBrowserPath(std::move(projectRelativePath));
	const std::string assetsPrefix = "Assets/";
	if (projectRelativePath == "Assets")
		return {};
	if (projectRelativePath.compare(0u, assetsPrefix.size(), assetsPrefix) == 0)
		return projectRelativePath.substr(assetsPrefix.size());
	return projectRelativePath;
}

std::string ProjectBrowserResourcePathForItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	if (!item.dragResourcePath.empty())
		return ProjectBrowserLegacyResourcePath(item.dragResourcePath);
	if (!item.selectionResourcePath.empty())
	{
		const auto subAssetDelimiter = item.selectionResourcePath.find('#');
		return ProjectBrowserLegacyResourcePath(
			subAssetDelimiter == std::string::npos
				? item.selectionResourcePath
				: item.selectionResourcePath.substr(0u, subAssetDelimiter));
	}
	return {};
}

std::string ProjectBrowserSelectionPathForItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	if (!item.selectionResourcePath.empty())
	{
		const auto subAssetDelimiter = item.selectionResourcePath.find('#');
		auto sourcePath = subAssetDelimiter == std::string::npos
			? item.selectionResourcePath
			: item.selectionResourcePath.substr(0u, subAssetDelimiter);
		auto legacyPath = ProjectBrowserLegacyResourcePath(std::move(sourcePath));
		if (subAssetDelimiter != std::string::npos)
			legacyPath += item.selectionResourcePath.substr(subAssetDelimiter);
		return legacyPath;
	}
	if (!item.dragResourcePath.empty())
		return ProjectBrowserLegacyResourcePath(item.dragResourcePath);
	return {};
}

std::filesystem::path ProjectBrowserAbsolutePathForResourcePath(
	const std::string& projectAssetsFolder,
	std::string resourcePath)
{
	if (resourcePath.empty())
		return {};
	if (resourcePath.front() == ':')
		return std::filesystem::path(EDITOR_EXEC(GetRealPath(resourcePath))).lexically_normal();
	if (std::filesystem::path(resourcePath).is_absolute())
		return std::filesystem::path(resourcePath).lexically_normal();

	resourcePath = NormalizeProjectBrowserPath(std::move(resourcePath));
	const auto projectRoot = ProjectRootFromAssetsFolder(projectAssetsFolder);
	if (resourcePath == "Assets" || resourcePath.compare(0u, 7u, "Assets/") == 0)
		return (projectRoot / resourcePath).lexically_normal();
	return std::filesystem::path(EDITOR_EXEC(GetRealPath(resourcePath))).lexically_normal();
}

std::string SanitizeAssetBrowserName(std::string value)
{
	value.erase(std::remove_if(value.begin(), value.end(), [](const auto& c)
	{
		return std::find(FILENAMES_CHARS.begin(), FILENAMES_CHARS.end(), c) == FILENAMES_CHARS.end();
	}), value.end());
	return value;
}

std::filesystem::path BuildUniqueAssetPath(
	const std::filesystem::path& folder,
	const std::string& requestedName,
	const std::string& extension)
{
	size_t suffix = 0u;
	for (;;)
	{
		const auto name = suffix == 0u
			? requestedName
			: requestedName + " (" + std::to_string(suffix) + ")";
		auto candidate = folder / (name + extension);
		if (!std::filesystem::exists(candidate))
			return candidate.lexically_normal();
		++suffix;
	}
}

bool IsPathInsideOrEqual(
	const std::filesystem::path& candidate,
	const std::filesystem::path& root)
{
	const auto normalizedCandidate = candidate.lexically_normal();
	const auto normalizedRoot = root.lexically_normal();
	if (normalizedCandidate == normalizedRoot)
		return true;
	const auto relative = normalizedCandidate.lexically_relative(normalizedRoot);
	if (relative.empty() || relative.is_absolute())
		return false;
	for (const auto& part : relative)
	{
		if (part == "..")
			return false;
	}
	return true;
}

void* ResolveAssetBrowserTextureHandle(
	NLS::Render::Resources::Texture2D* texture,
	const std::string& debugName)
{
	if (texture == nullptr || !NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
		return nullptr;

	const auto textureView = texture->GetOrCreateExplicitTextureView(debugName);
	if (textureView == nullptr)
		return nullptr;

	return NLS_SERVICE(NLS::UI::UIManager).ResolveTextureId(textureView);
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
	std::vector<std::filesystem::path> candidates;
	if (artifactPath.is_absolute())
	{
		candidates.push_back(artifactPath.lexically_normal());

		const auto artifactsRoot = projectRoot / "Library" / "Artifacts";
		std::vector<std::filesystem::path> parts;
		for (const auto& part : artifactPath.lexically_normal())
			parts.push_back(part);
		for (size_t index = 0u; index + 2u < parts.size(); ++index)
		{
			if (parts[index].generic_string() != "Artifacts")
				continue;

			std::filesystem::path remapped = artifactsRoot / parts[index + 1u];
			for (size_t relativeIndex = index + 2u; relativeIndex < parts.size(); ++relativeIndex)
				remapped /= parts[relativeIndex];
			remapped = remapped.lexically_normal();
			if (std::find(candidates.begin(), candidates.end(), remapped) == candidates.end())
				candidates.push_back(remapped);
		}
	}
	else
	{
		candidates.push_back((projectRoot / artifactPath).lexically_normal());
	}

	for (const auto& resolvedPath : candidates)
	{
		const auto relative = resolvedPath.lexically_relative(projectRoot.lexically_normal());
		if (relative.empty() || relative.is_absolute())
			continue;

		bool escapesProject = false;
		for (const auto& part : relative)
		{
			if (part == "..")
			{
				escapesProject = true;
				break;
			}
		}
		if (!escapesProject)
			return resolvedPath;
	}

	return {};
}

std::optional<std::string> SelectManifestPrefabSubAssetKeyForDragPayload(
	const std::filesystem::path& projectRoot,
	const nlohmann::json& manifest)
{
	const auto subAssets = manifest.find("subAssets");
	if (subAssets == manifest.end() || !subAssets->is_array())
		return std::nullopt;

	auto isUsablePrefabSubAsset = [&](const nlohmann::json& subAsset, const std::string* expectedKey)
	{
		if (!subAsset.is_object())
			return false;

		const auto subAssetKeyText = NLS::Editor::Assets::JsonString(subAsset, "subAssetKey");
		if (!subAssetKeyText.has_value() || subAssetKeyText->empty())
			return false;
		if (expectedKey != nullptr && *subAssetKeyText != *expectedKey)
			return false;

		const auto artifactTypeText = NLS::Editor::Assets::JsonStringOrDefault(subAsset, "artifactType");
		if (!artifactTypeText.has_value() ||
			(*artifactTypeText != "Prefab" && *artifactTypeText != "prefab"))
		{
			return false;
		}

		const auto resolvedArtifactPath = ResolveArtifactPathForManifest(projectRoot, subAsset);
		return !resolvedArtifactPath.empty() && std::filesystem::is_regular_file(resolvedArtifactPath);
	};

	const auto primarySubAssetKey = NLS::Editor::Assets::JsonString(manifest, "primarySubAssetKey");
	if (primarySubAssetKey.has_value() && !primarySubAssetKey->empty())
	{
		for (const auto& subAsset : *subAssets)
		{
			if (isUsablePrefabSubAsset(subAsset, &*primarySubAssetKey))
				return primarySubAssetKey;
		}
	}

	for (const auto& subAsset : *subAssets)
	{
		if (!isUsablePrefabSubAsset(subAsset, nullptr))
			continue;

		const auto subAssetKeyText = NLS::Editor::Assets::JsonString(subAsset, "subAssetKey");
		if (subAssetKeyText.has_value() && !subAssetKeyText->empty())
			return subAssetKeyText;
	}

	return std::nullopt;
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

const char* AssetDiagnosticSeverityLabel(const NLS::Core::Assets::AssetDiagnosticSeverity severity)
{
	using NLS::Core::Assets::AssetDiagnosticSeverity;
	switch (severity)
	{
	case AssetDiagnosticSeverity::Error:
		return "error";
	case AssetDiagnosticSeverity::Warning:
		return "warning";
	case AssetDiagnosticSeverity::Info:
	default:
		return "info";
	}
}

const char* ImportJobTerminalStatusLabel(const NLS::Editor::Assets::ImportJobTerminalStatus status)
{
	using NLS::Editor::Assets::ImportJobTerminalStatus;
	switch (status)
	{
	case ImportJobTerminalStatus::Succeeded:
		return "succeeded";
	case ImportJobTerminalStatus::Failed:
		return "failed";
	case ImportJobTerminalStatus::Cancelled:
		return "cancelled";
	case ImportJobTerminalStatus::None:
	default:
		return "running";
	}
}

void AppendUniqueDiagnostics(
	NLS::Core::Assets::AssetDiagnostics& diagnostics,
	const NLS::Core::Assets::AssetDiagnostics& incoming)
{
	for (const auto& diagnostic : incoming)
	{
		const auto duplicate = std::find_if(
			diagnostics.begin(),
			diagnostics.end(),
			[&diagnostic](const NLS::Core::Assets::AssetDiagnostic& existing)
			{
				return existing.severity == diagnostic.severity &&
					existing.code == diagnostic.code &&
					existing.path == diagnostic.path &&
					existing.message == diagnostic.message;
			});
		if (duplicate == diagnostics.end())
			diagnostics.push_back(diagnostic);
	}
}

void LogAssetPreimportFailureDetails(
	const NLS::Editor::Assets::AssetPreimportReason reason,
	const std::vector<std::filesystem::path>& changedPaths,
	const std::vector<NLS::Editor::Assets::ImportProgressEvent>& events,
	const NLS::Core::Assets::AssetDiagnostics& diagnostics)
{
	NLS_LOG_ERROR(std::string("Asset preimport failed after ") + AssetPreimportReasonLabel(reason));
	for (const auto& changedPath : changedPaths)
		NLS_LOG_ERROR("  changed path: " + changedPath.generic_string());

	for (const auto& event : events)
	{
		if (event.terminalStatus == NLS::Editor::Assets::ImportJobTerminalStatus::None)
			continue;

		NLS_LOG_ERROR(
			"  job " +
			std::to_string(event.jobId.value) +
			" " +
			ImportJobTerminalStatusLabel(event.terminalStatus) +
			": " +
			event.sourcePath +
			" - " +
			event.message);
	}

	for (const auto& diagnostic : diagnostics)
	{
		NLS_LOG_ERROR(
			std::string("  diagnostic [") +
			AssetDiagnosticSeverityLabel(diagnostic.severity) +
			"] " +
			diagnostic.code +
			" path=" +
			diagnostic.path.generic_string() +
			" message=" +
			diagnostic.message);
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
	bool previewPrefabReady = false;

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
			else if (fileType == Utils::PathParser::EFileType::MODEL)
			{
				if (auto manifestPrefabKey = SelectManifestPrefabSubAssetKeyForDragPayload(
					ProjectRootFromAssetsFolder(projectAssetsFolder),
					manifest);
					manifestPrefabKey.has_value())
				{
					subAssetKey = std::move(*manifestPrefabKey);
				}
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
					previewPrefabReady =
						imported &&
						artifactType == NLS::Core::Assets::ArtifactType::Prefab;
					break;
				}

				if (previewPrefabReady && fileType == Utils::PathParser::EFileType::MODEL)
				{
					for (const auto& subAsset : *subAssets)
					{
						const auto artifactTypeText = JsonStringOrDefault(subAsset, "artifactType");
						if (!artifactTypeText.has_value())
							continue;

						const bool rendererDependency =
							artifactTypeText == "Mesh" ||
							artifactTypeText == "mesh" ||
							artifactTypeText == "Material" ||
							artifactTypeText == "material" ||
							artifactTypeText == "Texture" ||
							artifactTypeText == "texture";
						if (!rendererDependency)
							continue;

						const auto resolvedArtifactPath =
							ResolveArtifactPathForManifest(ProjectRootFromAssetsFolder(projectAssetsFolder), subAsset);
						if (resolvedArtifactPath.empty() || !std::filesystem::is_regular_file(resolvedArtifactPath))
						{
							previewPrefabReady = false;
							break;
						}
					}
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
		imported,
		previewPrefabReady);
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
			auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
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
	NLS::Editor::Assets::SetObjectReferencePickerEntriesProvider([this]()
	{
		return m_projectAssetDatabaseReady && m_projectAssetDatabase
			? NLS::Editor::Assets::BuildObjectReferencePickerEntries(*m_projectAssetDatabase)
			: std::vector<NLS::Editor::Assets::ObjectReferencePickerEntry> {};
	});

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

	m_assetList = &CreateWidget<Group>();

	Fill();
}

Editor::Panels::AssetBrowser::~AssetBrowser()
{
	DiscardObjectReferencePickerEntriesRefresh();
	NLS::Editor::Assets::SetObjectReferencePickerEntriesProvider({});
	NLS::Editor::Assets::SetObjectReferencePickerEntries({});
	DestroyCachedThumbnailTextures(true);
}

void Editor::Panels::AssetBrowser::Fill()
{
	RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
		NLS::Editor::Assets::AssetBrowserRefreshReason::InitialBuild));
}

void Editor::Panels::AssetBrowser::Clear()
{
	if (m_assetList != nullptr)
		m_assetList->RemoveAllWidgets();
	DestroyCachedThumbnailTextures(false);
	m_thumbnailResultsByItemKey.clear();
	m_thumbnailItemKeyByCacheKey.clear();
	m_lastThumbnailRequestSize = 0u;
	m_lastThumbnailGenerationScopeKey.clear();
	m_thumbnailGenerationScopeDirty = true;
	m_thumbnailService.ClearQueuedRequests();
}

void Editor::Panels::AssetBrowser::Refresh()
{
	RefreshPreservingExpandedFolders();
}

void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()
{
	PumpQueuedCachedThumbnailTextureLoads();
	PumpRetiredProjectAssetDatabaseRefreshes();
	if (m_projectAssetDatabaseRefresh.has_value())
	{
		auto& refresh = *m_projectAssetDatabaseRefresh;
		if (!refresh.future.valid())
		{
			m_projectAssetDatabaseRefresh.reset();
		}
		else if (refresh.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			std::unique_ptr<NLS::Editor::Assets::AssetDatabaseFacade> database;
			try
			{
				database = refresh.future.get();
			}
			catch (...)
			{
				database.reset();
			}

			const auto refreshRoot = refresh.root.lexically_normal();
			m_projectAssetDatabaseRefresh.reset();
			if (!refreshRoot.empty() &&
				refreshRoot == m_projectAssetDatabaseRoot.lexically_normal() &&
				database)
			{
				DiscardObjectReferencePickerEntriesRefresh();
				m_projectAssetDatabase = std::move(database);
				m_projectAssetDatabaseReady = true;
				RequestObjectReferencePickerEntriesRefresh();
				RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
					NLS::Editor::Assets::AssetBrowserRefreshReason::AssetDatabaseReady));
			}
			if (m_projectAssetDatabaseRefreshQueuedAfterInFlight)
			{
				m_projectAssetDatabaseRefreshQueuedAfterInFlight = false;
				RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
					NLS::Editor::Assets::AssetBrowserRefreshReason::AssetDatabaseMutation));
			}
		}
	}
	PumpObjectReferencePickerEntriesRefresh();

	if (!m_watchersStartupQueued)
		StartWatchersAsync();

	CompleteWatcherStartupIfReady();
	if (m_startupWatcherPreimportGateOpen)
		ConsumeWatcherChangesAndSchedulePreimport();

	if (m_refreshRequested)
	{
		m_refreshRequested = false;
		m_projectFolderTreeRefreshRequested = false;
		RefreshPreservingExpandedFolders();
	}
	else if (m_projectFolderTreeRefreshRequested)
	{
		m_projectFolderTreeRefreshRequested = false;
		RebuildProjectFolderTreePresentation();
	}
}

void Editor::Panels::AssetBrowser::OnAfterDrawWidgets()
{
	DrawProjectAssetBrowser();
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
		NLS::Core::Assets::AssetDiagnostics diagnostics;
		AssetPreimportScheduler preimportScheduler;
		const auto imported = preimportScheduler.Run(database, tracker, request);
		AppendUniqueDiagnostics(diagnostics, database.GetDiagnostics());
		EDITOR_EXEC(DelayAction([
			reason = request.reason,
			changedPaths = request.changedPaths,
			imported,
			diagnostics = std::move(diagnostics)]
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
				LogAssetPreimportFailureDetails(reason, changedPaths, {}, diagnostics);
			}
		}));
	}));
}

void Editor::Panels::AssetBrowser::RefreshPreservingExpandedFolders()
{
	Clear();
	RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
		NLS::Editor::Assets::AssetBrowserRefreshReason::AssetDatabaseMutation));
}

void Editor::Panels::AssetBrowser::RebuildProjectFolderTreePresentation()
{
	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	AddProjectBrowserAncestorFolders(m_expandedProjectFolders, m_selectedProjectFolder);
	NLS::Editor::Assets::AssetBrowserFolderTreeBuildOptions treeOptions;
	treeOptions.expandedFolders = m_expandedProjectFolders;
	treeOptions.selectedFolder = m_selectedProjectFolder;
	m_projectFolderTree = NLS::Editor::Assets::BuildProjectAssetFolderTree(projectRoot, treeOptions);
}

void Editor::Panels::AssetBrowser::RebuildProjectAssetPresentation(
	const NLS::Editor::Assets::AssetBrowserRefreshPlan refreshPlan)
{
	++m_projectAssetPresentationGeneration;
	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	const auto resolved = NLS::Editor::Assets::ResolveAssetBrowserFolderSelection(
		projectRoot,
		m_selectedProjectFolder);
	m_selectedProjectFolder = resolved.projectRelativePath;
	m_currentBreadcrumb = NLS::Editor::Assets::BuildAssetBrowserBreadcrumb(m_selectedProjectFolder);

	if (refreshPlan.rebuildFolderTree || m_projectFolderTree.projectRelativePath.empty())
	{
		RebuildProjectFolderTreePresentation();
	}

	if (projectRoot.empty())
	{
		DiscardObjectReferencePickerEntriesRefresh();
		m_projectAssetDatabase.reset();
		m_projectAssetDatabaseRoot.clear();
		m_projectAssetDatabaseReady = false;
		DiscardProjectAssetDatabaseRefresh();
		m_projectAssetDatabaseRefreshQueuedAfterInFlight = false;
		NLS::Editor::Assets::SetObjectReferencePickerEntries({});
		++m_objectReferencePickerRefreshGeneration;
		m_objectReferencePickerRefreshRequested = false;
	}
	else if (m_projectAssetDatabaseRoot.lexically_normal() != projectRoot.lexically_normal())
	{
		DiscardObjectReferencePickerEntriesRefresh();
		m_projectAssetDatabaseRoot = projectRoot.lexically_normal();
		m_projectAssetDatabase.reset();
		m_projectAssetDatabaseReady = false;
		DiscardProjectAssetDatabaseRefresh();
		m_projectAssetDatabaseRefreshQueuedAfterInFlight = false;
		NLS::Editor::Assets::SetObjectReferencePickerEntries({});
		++m_objectReferencePickerRefreshGeneration;
		m_objectReferencePickerRefreshRequested = false;
	}

	const auto refreshScheduling = NLS::Editor::Assets::PlanAssetDatabaseRefreshScheduling(
		projectRoot.empty(),
		refreshPlan.refreshAssetDatabase,
		m_projectAssetDatabaseReady,
		m_projectAssetDatabaseRefresh.has_value() &&
			m_projectAssetDatabaseRefresh->root.lexically_normal() == projectRoot.lexically_normal());
	if (refreshScheduling.queueRefreshAfterInFlight)
		m_projectAssetDatabaseRefreshQueuedAfterInFlight = true;
	if (refreshScheduling.startRefresh)
	{
		DiscardObjectReferencePickerEntriesRefresh();
		m_projectAssetDatabaseReady = false;
		try
		{
			m_projectAssetDatabaseRefresh = AssetDatabaseRefresh {
				projectRoot.lexically_normal(),
				std::async(
					std::launch::async,
					[projectRoot = projectRoot.lexically_normal()]() -> std::unique_ptr<NLS::Editor::Assets::AssetDatabaseFacade>
					{
						auto database = std::make_unique<NLS::Editor::Assets::AssetDatabaseFacade>(
							NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
						if (!database->Refresh())
							return {};
						return database;
					})
			};
		}
		catch (const std::exception& exception)
		{
			DiscardProjectAssetDatabaseRefresh();
			NLS_LOG_ERROR(std::string("Asset Browser database refresh failed to start: ") + exception.what());
		}
		catch (...)
		{
			DiscardProjectAssetDatabaseRefresh();
			NLS_LOG_ERROR("Asset Browser database refresh failed to start.");
		}
	}

	NLS::Editor::Assets::AssetBrowserBuildOptions buildOptions;
	buildOptions.includeGeneratedSubAssets = true;
	buildOptions.verifyGeneratedSubAssetManifests = false;
	buildOptions.searchQuery = m_projectSearchQuery;
	buildOptions.typeFilter = m_projectTypeFilter;
	if (refreshPlan.rebuildCurrentFolderItems)
	{
		NLS::Editor::Assets::AssetBrowserBuildOptions unfilteredBuildOptions;
		unfilteredBuildOptions.includeGeneratedSubAssets = true;
		unfilteredBuildOptions.verifyGeneratedSubAssetManifests = false;
		unfilteredBuildOptions.loadSourceAssetMetadataWithoutDatabase = m_projectAssetDatabaseReady && m_projectAssetDatabase;
		m_unfilteredCurrentFolderItems = NLS::Editor::Assets::BuildCurrentFolderAssetItems(
			projectRoot,
			m_selectedProjectFolder,
			m_projectAssetDatabaseReady && m_projectAssetDatabase
				? m_projectAssetDatabase.get()
				: nullptr,
			unfilteredBuildOptions);
	}
	m_currentFolderItems = NLS::Editor::Assets::FilterAssetBrowserItems(
		m_unfilteredCurrentFolderItems,
		buildOptions);
	m_visibleThumbnailItems.clear();
	m_visibleThumbnailItemsKnown = false;
	m_thumbnailGenerationScopeDirty = true;
}

void Editor::Panels::AssetBrowser::SelectProjectFolder(const std::string& projectRelativePath)
{
	const auto requested = NormalizeProjectBrowserPath(projectRelativePath);
	if (m_selectedProjectFolder == requested)
		return;

	m_selectedProjectFolder = requested;
	m_selectedProjectItem.clear();
	AddProjectBrowserAncestorFolders(m_expandedProjectFolders, m_selectedProjectFolder);
	RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
		NLS::Editor::Assets::AssetBrowserRefreshReason::FolderSelection));
}

void Editor::Panels::AssetBrowser::DrawProjectAssetBrowser()
{
	const auto thumbnailTextureFramePlan = NLS::Editor::Assets::BeginAssetBrowserThumbnailTextureFrame(
		std::move(m_thumbnailTexturesUsedThisFrame),
		std::move(m_thumbnailTexturesPendingRelease));
	m_thumbnailTexturesUsedThisFrame = std::move(thumbnailTextureFramePlan.usedThisFrame);
	m_thumbnailTexturesPendingRelease = std::move(thumbnailTextureFramePlan.pendingRelease);
	for (const auto& key : thumbnailTextureFramePlan.releaseNow)
		ReleaseCachedThumbnailTexture(key);

	const float availableHeight = ImGui::GetContentRegionAvail().y;
	if (availableHeight <= 0.0f)
		return;

	const float treeWidth = (std::max)(180.0f, (std::min)(320.0f, ImGui::GetContentRegionAvail().x * 0.30f));
	ImGui::BeginChild("##AssetBrowserFolderTree", ImVec2(treeWidth, availableHeight), true);
	if (m_projectFolderTree.projectRelativePath.empty())
		RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
			NLS::Editor::Assets::AssetBrowserRefreshReason::InitialBuild));
		(void)DrawProjectFolderTree(m_projectFolderTree);
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##AssetBrowserContent", ImVec2(0.0f, availableHeight), true);
	DrawProjectBreadcrumb();
	DrawProjectFilterBar();
	ImGui::Separator();
	const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	ImGui::BeginChild("##AssetBrowserCurrentFolder", ImVec2(0.0f, -footerHeight), false);
	DrawCurrentFolderGrid();
	if (ImGui::BeginPopupContextWindow(
			"##AssetBrowserCurrentFolderContext",
			ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		const auto currentFolder = NormalizeProjectBrowserPath(m_selectedProjectFolder);
		DrawProjectCurrentFolderContextMenu(
			"##currentFolderContent",
			currentFolder,
			ProjectBrowserAbsolutePathForResourcePath(m_projectAssetFolder, currentFolder));
		ImGui::EndPopup();
	}
	ImGui::EndChild();
	DrawProjectBrowserTextDialog();
	ImGui::Separator();
	DrawAssetBrowserFooter();
	UpdateThumbnailGenerationScope();
	PruneCachedThumbnailTextures();
	ImGui::EndChild();
}

bool Editor::Panels::AssetBrowser::DrawProjectFolderTree(
	const NLS::Editor::Assets::AssetBrowserFolderNode& node)
{
	if (node.projectRelativePath.empty())
		return true;

	const bool selected = m_selectedProjectFolder == node.projectRelativePath;
	const bool openedBySelection = IsProjectBrowserAncestorOf(node.projectRelativePath, m_selectedProjectFolder);
	const bool hasExpandableChildren = node.hasChildren || !node.children.empty();
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
	if (!hasExpandableChildren)
		flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (selected)
		flags |= ImGuiTreeNodeFlags_Selected;
	if (openedBySelection)
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);

	const bool opened = ImGui::TreeNodeEx(
		node.projectRelativePath.c_str(),
		flags,
		"%s",
		node.displayName.c_str());
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
	{
		const auto selectedProjectFolder = node.projectRelativePath;
		const bool selectionWillChange =
			NLS::Editor::Assets::ShouldStopDrawingAssetBrowserFolderNodeAfterSelection(
				m_selectedProjectFolder,
				selectedProjectFolder);
		SelectProjectFolder(selectedProjectFolder);
		if (selectionWillChange)
		{
			if (opened && hasExpandableChildren)
				ImGui::TreePop();
			return false;
		}
	}
	if (opened && hasExpandableChildren && !node.childrenEnumerated)
	{
		m_expandedProjectFolders.insert(node.projectRelativePath);
		m_projectFolderTreeRefreshRequested = true;
	}
	const auto presentationGeneration = m_projectAssetPresentationGeneration;
	DrawProjectFolderContextMenu(
		"##folderTreeContext",
		node.projectRelativePath,
		node.absolutePath);
	if (presentationGeneration != m_projectAssetPresentationGeneration)
	{
		if (opened && hasExpandableChildren)
			ImGui::TreePop();
		return false;
	}
	DrawProjectFolderDropTarget(node.projectRelativePath, node.absolutePath);
	if (presentationGeneration != m_projectAssetPresentationGeneration)
	{
		if (opened && hasExpandableChildren)
			ImGui::TreePop();
		return false;
	}

	if (hasExpandableChildren && opened)
	{
		for (const auto& child : node.children)
		{
			if (!DrawProjectFolderTree(child))
			{
				ImGui::TreePop();
				return false;
			}
		}
		ImGui::TreePop();
	}
	return true;
}

void Editor::Panels::AssetBrowser::DrawProjectFilterBar()
{
	bool filtersChanged = false;
	std::array<char, 256u> searchBuffer {};
	const auto copyCount = (std::min)(m_projectSearchQuery.size(), searchBuffer.size() - 1u);
	std::copy_n(m_projectSearchQuery.data(), copyCount, searchBuffer.data());

	ImGui::SetNextItemWidth((std::max)(160.0f, ImGui::GetContentRegionAvail().x - 160.0f));
	if (ImGui::InputTextWithHint(
			"##AssetBrowserSearch",
			"Search",
			searchBuffer.data(),
			searchBuffer.size()))
	{
		m_projectSearchQuery = searchBuffer.data();
		filtersChanged = true;
	}

	ImGui::SameLine();
	ImGui::SetNextItemWidth(140.0f);
	if (ImGui::BeginCombo("##AssetBrowserTypeFilter", NLS::Editor::Assets::AssetBrowserItemTypeDisplayLabel(m_projectTypeFilter)))
	{
		for (const auto type : NLS::Editor::Assets::AssetBrowserItemTypeFilterOptions())
		{
			const bool selected = m_projectTypeFilter == type;
			if (ImGui::Selectable(NLS::Editor::Assets::AssetBrowserItemTypeDisplayLabel(type), selected))
			{
				if (m_projectTypeFilter != type)
				{
					m_projectTypeFilter = type;
					filtersChanged = true;
				}
			}
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	if (filtersChanged)
	{
		m_selectedProjectItem.clear();
		RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
			NLS::Editor::Assets::AssetBrowserRefreshReason::FilterChange));
	}
}

void Editor::Panels::AssetBrowser::DrawAssetBrowserFooter()
{
	const float iconSize = 14.0f;
	const float sliderWidth = 120.0f;
	const float controlWidth = iconSize + ImGui::GetStyle().ItemSpacing.x +
		sliderWidth + ImGui::GetStyle().ItemSpacing.x + iconSize;
	const float x = (std::max)(0.0f, ImGui::GetContentRegionAvail().x - controlWidth);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x);
	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	auto* drawList = ImGui::GetWindowDrawList();
	const auto iconColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	drawList->AddRect(
		ImVec2(cursor.x + 2.0f, cursor.y + 4.0f),
		ImVec2(cursor.x + 11.0f, cursor.y + 13.0f),
		iconColor,
		1.0f);
	for (int row = 0; row < 3; ++row)
	{
		drawList->AddLine(
			ImVec2(cursor.x + 3.0f, cursor.y + 5.0f + row * 3.0f),
			ImVec2(cursor.x + 10.0f, cursor.y + 5.0f + row * 3.0f),
			iconColor);
	}
	ImGui::Dummy(ImVec2(iconSize, ImGui::GetFrameHeight()));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(sliderWidth);
	ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 6.0f);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetColorU32(ImGuiCol_FrameBg));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(73, 140, 224, 255));
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(98, 164, 244, 255));
	if (ImGui::SliderFloat("##AssetBrowserThumbnailSize", &m_thumbnailSize, 64.0f, 160.0f, ""))
	{
		m_thumbnailGenerationScopeDirty = true;
	}
	ImGui::PopStyleColor(4);
	ImGui::PopStyleVar();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Thumbnail Size");
	ImGui::SameLine();
	const ImVec2 gridCursor = ImGui::GetCursorScreenPos();
	const float cell = 5.0f;
	for (int y = 0; y < 2; ++y)
	{
		for (int xCell = 0; xCell < 2; ++xCell)
		{
			const ImVec2 min(gridCursor.x + xCell * (cell + 2.0f), gridCursor.y + 4.0f + y * (cell + 2.0f));
			drawList->AddRect(min, ImVec2(min.x + cell, min.y + cell), iconColor, 1.0f);
		}
	}
	ImGui::Dummy(ImVec2(iconSize, ImGui::GetFrameHeight()));
}

void Editor::Panels::AssetBrowser::DrawProjectBreadcrumb()
{
	for (size_t index = 0u; index < m_currentBreadcrumb.size(); ++index)
	{
		if (index > 0u)
		{
			ImGui::SameLine();
			ImGui::TextUnformatted(">");
			ImGui::SameLine();
		}

		const auto& segment = m_currentBreadcrumb[index];
		if (ImGui::SmallButton((segment.displayName + "##breadcrumb_" + segment.projectRelativePath).c_str()))
			SelectProjectFolder(segment.projectRelativePath);
	}
}

void Editor::Panels::AssetBrowser::RequestProjectBrowserTextDialog(
	const ProjectBrowserTextDialogKind kind,
	std::string title,
	const std::filesystem::path& targetAbsoluteFolder,
	std::string targetProjectRelativeFolder,
	const std::filesystem::path& sourceAbsolutePath,
	std::string defaultName)
{
	m_projectBrowserTextDialog = {};
	m_projectBrowserTextDialog.kind = kind;
	m_projectBrowserTextDialog.title = std::move(title);
	m_projectBrowserTextDialog.targetAbsoluteFolder = targetAbsoluteFolder.lexically_normal();
	m_projectBrowserTextDialog.targetProjectRelativeFolder = NormalizeProjectBrowserPath(std::move(targetProjectRelativeFolder));
	m_projectBrowserTextDialog.sourceAbsolutePath = sourceAbsolutePath.lexically_normal();
	m_projectBrowserTextDialog.requestOpen = true;
	defaultName = SanitizeAssetBrowserName(std::move(defaultName));
	const auto copyCount = (std::min)(
		defaultName.size(),
		m_projectBrowserTextDialog.buffer.size() - 1u);
	std::copy_n(defaultName.data(), copyCount, m_projectBrowserTextDialog.buffer.data());
}

void Editor::Panels::AssetBrowser::DrawProjectBrowserTextDialog()
{
	if (m_projectBrowserTextDialog.kind == ProjectBrowserTextDialogKind::None)
		return;

	if (m_projectBrowserTextDialog.requestOpen)
	{
		ImGui::OpenPopup(m_projectBrowserTextDialog.title.c_str());
		m_projectBrowserTextDialog.requestOpen = false;
	}

	bool close = false;
	if (ImGui::BeginPopupModal(
			m_projectBrowserTextDialog.title.c_str(),
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::SetNextItemWidth(280.0f);
		const bool enterPressed = ImGui::InputText(
			"Name",
			m_projectBrowserTextDialog.buffer.data(),
			m_projectBrowserTextDialog.buffer.size(),
			ImGuiInputTextFlags_EnterReturnsTrue);
		if (enterPressed || ImGui::Button("OK"))
		{
			close = CommitProjectBrowserTextDialog();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			close = true;

		if (close)
		{
			m_projectBrowserTextDialog = {};
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

bool Editor::Panels::AssetBrowser::CommitProjectBrowserTextDialog()
{
	const auto kind = m_projectBrowserTextDialog.kind;
	auto name = SanitizeAssetBrowserName(m_projectBrowserTextDialog.buffer.data());
	if (name.empty())
		return false;

	auto createdOrChangedProjectPath = std::filesystem::path {};
	auto targetFolder = m_projectBrowserTextDialog.targetAbsoluteFolder.lexically_normal();
	if (targetFolder.empty())
		targetFolder = (ProjectRootFromAssetsFolder(m_projectAssetFolder) / m_selectedProjectFolder).lexically_normal();

	auto createTextAsset = [&](const std::string& extension, const std::string& contents)
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, extension);
		std::filesystem::create_directories(finalPath.parent_path());
		std::ofstream output(finalPath, std::ios::binary | std::ios::trunc);
		output << contents << std::endl;
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
	};

	switch (kind)
	{
	case ProjectBrowserTextDialogKind::RenameFolder:
	{
		const auto source = m_projectBrowserTextDialog.sourceAbsolutePath.lexically_normal();
		const auto destination = (source.parent_path() / name).lexically_normal();
		if (source == destination || std::filesystem::exists(destination))
			return false;
		RenameAsset(source.string(), EnsureTrailingPathSeparator(destination));
		EDITOR_EXEC(PropagateFolderRename(source.string(), EnsureTrailingPathSeparator(destination)));
		createdOrChangedProjectPath = EditorAssetFolderFromAbsolutePath(m_projectAssetFolder, destination.string());
		if (m_selectedProjectFolder == m_projectBrowserTextDialog.targetProjectRelativeFolder)
			m_selectedProjectFolder = NormalizeProjectBrowserPath(createdOrChangedProjectPath);
		break;
	}
	case ProjectBrowserTextDialogKind::RenameFile:
	{
		const auto source = m_projectBrowserTextDialog.sourceAbsolutePath.lexically_normal();
		const auto destination = (source.parent_path() / (name + source.extension().string())).lexically_normal();
		if (source == destination || std::filesystem::exists(destination))
			return false;
		RenameAsset(source.string(), destination.string());
		EDITOR_EXEC(PropagateFileRename(source.string(), destination.string()));
		if (EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath() == source.string())
			EDITOR_CONTEXT(sceneManager).StoreCurrentSceneSourcePath(destination.string());
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, destination.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateFolder:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, "");
		std::filesystem::create_directories(finalPath);
		createdOrChangedProjectPath = EditorAssetFolderFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateScene:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".scene");
		Engine::SceneSystem::Scene scene;
		if (!Engine::SceneSystem::SceneManager::SaveSceneToPath(scene, finalPath.string()))
		{
			NLS_LOG_ERROR("Failed to create scene asset: " + finalPath.string());
			return false;
		}
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateStandardShader:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".hlsl");
		std::filesystem::copy_file(
			EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\Standard.hlsl",
			finalPath,
			std::filesystem::copy_options::overwrite_existing);
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateStandardPBRShader:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".hlsl");
		std::filesystem::copy_file(
			EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\Standard.hlsl",
			finalPath,
			std::filesystem::copy_options::overwrite_existing);
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateUnlitShader:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".hlsl");
		std::filesystem::copy_file(
			EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\Unlit.hlsl",
			finalPath,
			std::filesystem::copy_options::overwrite_existing);
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateLambertShader:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".hlsl");
		std::filesystem::copy_file(
			EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\Lambert.hlsl",
			finalPath,
			std::filesystem::copy_options::overwrite_existing);
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateEmptyMaterial:
		createTextAsset(".mat", "<root><shader>?</shader></root>");
		break;
	case ProjectBrowserTextDialogKind::CreateStandardMaterial:
	case ProjectBrowserTextDialogKind::CreateStandardPBRMaterial:
		createTextAsset(".mat", "<root><shader>:Shaders\\Standard.hlsl</shader></root>");
		break;
	case ProjectBrowserTextDialogKind::CreateUnlitMaterial:
		createTextAsset(".mat", "<root><shader>:Shaders\\Unlit.hlsl</shader></root>");
		break;
	case ProjectBrowserTextDialogKind::CreateLambertMaterial:
		createTextAsset(".mat", "<root><shader>:Shaders\\Lambert.hlsl</shader></root>");
		break;
	case ProjectBrowserTextDialogKind::None:
	default:
		return false;
	}

	if (!createdOrChangedProjectPath.empty())
		ScheduleProjectAssetPreimportForPath(createdOrChangedProjectPath);
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

void Editor::Panels::AssetBrowser::DrawProjectFolderContextMenu(
	const std::string& popupId,
	const std::string& projectRelativeFolder,
	const std::filesystem::path& absoluteFolder)
{
	const auto scopedPopupId = popupId + "_" + NormalizeProjectBrowserPath(projectRelativeFolder);
	if (!ImGui::BeginPopupContextItem(scopedPopupId.c_str()))
		return;

	DrawProjectCurrentFolderContextMenu("##folderContent", projectRelativeFolder, absoluteFolder);

	const bool isAssetsRoot = NormalizeProjectBrowserPath(projectRelativeFolder) == "Assets";
	if (ImGui::MenuItem("Rename...", nullptr, false, !isAssetsRoot))
	{
		RequestProjectBrowserTextDialog(
			ProjectBrowserTextDialogKind::RenameFolder,
			"Rename Folder",
			absoluteFolder.parent_path(),
			projectRelativeFolder,
			absoluteFolder,
			absoluteFolder.filename().generic_string());
	}

	if (ImGui::MenuItem("Delete", nullptr, false, !isAssetsRoot))
	{
		using namespace NLS::Dialogs;
		MessageBox message(
			"Delete folder",
			"Deleting a folder (and all its content) is irreversible, are you sure that you want to delete \"" + absoluteFolder.string() + "\"?",
			MessageBox::EMessageType::WARNING,
			MessageBox::EButtonLayout::YES_NO);
		if (message.GetUserAction() == MessageBox::EUserAction::YES)
		{
			EDITOR_EXEC(PropagateFolderDestruction(absoluteFolder.string()));
			std::filesystem::remove_all(absoluteFolder);
			if (IsProjectBrowserAncestorOf(projectRelativeFolder, m_selectedProjectFolder))
				m_selectedProjectFolder = "Assets";
			ScheduleProjectAssetPreimportForPath(projectRelativeFolder);
			RebuildProjectAssetPresentationAfterWorkflow();
		}
	}

	ImGui::EndPopup();
}

void Editor::Panels::AssetBrowser::DrawProjectCurrentFolderContextMenu(
	const std::string& popupId,
	const std::string& projectRelativeFolder,
	const std::filesystem::path& absoluteFolder)
{
	(void)popupId;

	if (ImGui::MenuItem("Show in explorer"))
		Platform::SystemCalls::ShowInExplorer(EnsureTrailingPathSeparator(absoluteFolder));

	if (ImGui::MenuItem("Import Here..."))
	{
		if (EDITOR_EXEC(ImportAssetAtLocation(EnsureTrailingPathSeparator(absoluteFolder))))
			RebuildProjectAssetPresentationAfterWorkflow();
	}

	if (ImGui::BeginMenu("Create"))
	{
		if (ImGui::MenuItem("Folder"))
		{
			RequestProjectBrowserTextDialog(
				ProjectBrowserTextDialogKind::CreateFolder,
				"Create Folder",
				absoluteFolder,
				projectRelativeFolder,
				{},
				"New Folder");
		}
		if (ImGui::MenuItem("Scene"))
		{
			RequestProjectBrowserTextDialog(
				ProjectBrowserTextDialogKind::CreateScene,
				"Create Scene",
				absoluteFolder,
				projectRelativeFolder,
				{},
				"New Scene");
		}
		if (ImGui::BeginMenu("Shader"))
		{
			if (ImGui::MenuItem("Standard template"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateStandardShader, "Create Standard Shader", absoluteFolder, projectRelativeFolder, {}, "New Shader");
			if (ImGui::MenuItem("Standard PBR template"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateStandardPBRShader, "Create Standard PBR Shader", absoluteFolder, projectRelativeFolder, {}, "New Shader");
			if (ImGui::MenuItem("Unlit template"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateUnlitShader, "Create Unlit Shader", absoluteFolder, projectRelativeFolder, {}, "New Shader");
			if (ImGui::MenuItem("Lambert template"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateLambertShader, "Create Lambert Shader", absoluteFolder, projectRelativeFolder, {}, "New Shader");
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Material"))
		{
			if (ImGui::MenuItem("Empty"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateEmptyMaterial, "Create Empty Material", absoluteFolder, projectRelativeFolder, {}, "New Material");
			if (ImGui::MenuItem("Standard"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateStandardMaterial, "Create Standard Material", absoluteFolder, projectRelativeFolder, {}, "New Material");
			if (ImGui::MenuItem("Standard PBR"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateStandardPBRMaterial, "Create Standard PBR Material", absoluteFolder, projectRelativeFolder, {}, "New Material");
			if (ImGui::MenuItem("Unlit"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateUnlitMaterial, "Create Unlit Material", absoluteFolder, projectRelativeFolder, {}, "New Material");
			if (ImGui::MenuItem("Lambert"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateLambertMaterial, "Create Lambert Material", absoluteFolder, projectRelativeFolder, {}, "New Material");
			ImGui::EndMenu();
		}
		ImGui::EndMenu();
	}
}

void Editor::Panels::AssetBrowser::DrawProjectGridItemContextMenu(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	const auto capabilities = NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(item);
	if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
	{
		DrawProjectFolderContextMenu(
			"##gridFolderContext",
			item.projectRelativePath,
			item.absolutePath);
		return;
	}

	if (!ImGui::BeginPopupContextItem("##gridItemContext"))
		return;

	if (capabilities.canOpenExternal && ImGui::MenuItem("Open"))
		Platform::SystemCalls::OpenFile(item.absolutePath.string());
	if (capabilities.canEdit && ImGui::MenuItem("Edit"))
		OpenProjectGridItem(item);
	if (capabilities.canPreview && ImGui::MenuItem("Preview"))
		PreviewProjectGridItem(item);
	if (capabilities.canReimport && ImGui::MenuItem("Reimport"))
		ReimportProjectAssetAsync(m_projectAssetFolder, item.absolutePath.string());
	if (capabilities.canReload && ImGui::MenuItem("Reload"))
	{
		const auto resourcePath = ProjectBrowserResourcePathForItem(item);
		if (item.type == NLS::Editor::Assets::AssetBrowserItemType::Texture)
		{
			auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
			if (textureManager.IsResourceRegistered(resourcePath))
			{
				textureManager.AResourceManager::ReloadResource(resourcePath);
				EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor").Refresh();
			}
		}
		else if (item.type == NLS::Editor::Assets::AssetBrowserItemType::Material)
		{
			auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
			if (materialManager[resourcePath] != nullptr)
			{
				materialManager.AResourceManager::ReloadResource(resourcePath);
				EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor").Refresh();
			}
		}
	}
	if (capabilities.canCompile && ImGui::MenuItem("Compile"))
	{
		const auto resourcePath = ProjectBrowserResourcePathForItem(item);
		auto& shaderManager = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager);
		if (shaderManager.IsResourceRegistered(resourcePath))
		{
			Render::Resources::Loaders::ShaderLoader::Recompile(
				*shaderManager[resourcePath],
				item.absolutePath.string(),
				NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());
		}
		else if (NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager)[resourcePath] != nullptr)
		{
			NLS_LOG_INFO("[COMPILE] \"" + item.absolutePath.string() + "\": Success!");
		}
	}
	if (capabilities.canDuplicate && ImGui::MenuItem("Duplicate"))
	{
		const auto source = item.absolutePath.lexically_normal();
		const auto stem = source.stem().string();
		const auto extension = source.extension().string();
		const auto destination = BuildUniqueAssetPath(source.parent_path(), stem, extension);
		std::filesystem::copy_file(source, destination);
		ScheduleProjectAssetPreimportForPath(EditorAssetPathFromAbsolutePath(m_projectAssetFolder, destination.string()));
		RebuildProjectAssetPresentationAfterWorkflow();
	}
	if (capabilities.canRename && ImGui::MenuItem("Rename..."))
	{
		RequestProjectBrowserTextDialog(
			ProjectBrowserTextDialogKind::RenameFile,
			"Rename Asset",
			item.absolutePath.parent_path(),
			m_selectedProjectFolder,
			item.absolutePath,
			item.absolutePath.stem().generic_string());
	}
	if (capabilities.canDelete && ImGui::MenuItem("Delete"))
	{
		using namespace NLS::Dialogs;
		MessageBox message(
			"Delete file",
			"Deleting a file is irreversible, are you sure that you want to delete \"" + item.absolutePath.string() + "\"?",
			MessageBox::EMessageType::WARNING,
			MessageBox::EButtonLayout::YES_NO);
		if (message.GetUserAction() == MessageBox::EUserAction::YES)
		{
			RemoveAsset(item.absolutePath.string());
			EDITOR_EXEC(PropagateFileRename(item.absolutePath.string(), "?"));
			if (EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath() == item.absolutePath.string())
				EDITOR_CONTEXT(sceneManager).ForgetCurrentSceneSourcePath();
			ScheduleProjectAssetPreimportForPath(item.projectRelativePath);
			RebuildProjectAssetPresentationAfterWorkflow();
		}
	}
	if (capabilities.canOpenProperties && ImGui::MenuItem("Properties"))
		OpenProjectGridItemProperties(item);

	ImGui::EndPopup();
}

void Editor::Panels::AssetBrowser::DrawCurrentFolderGrid()
{
	const auto displayItems = NLS::Editor::Assets::BuildAssetBrowserDisplayItems(
		m_currentFolderItems,
		m_expandedProjectAssetItems);
	if (NLS::Editor::Assets::ResolveAssetBrowserContentViewMode(m_thumbnailSize) ==
		NLS::Editor::Assets::AssetBrowserContentViewMode::List)
	{
		DrawCurrentFolderList(displayItems);
		return;
	}

	const float cellWidth = (std::max)(96.0f, m_thumbnailSize + 28.0f);
	const float availableWidth = ImGui::GetContentRegionAvail().x;
	const int columns = (std::max)(1, static_cast<int>(std::floor(availableWidth / cellWidth)));
	const float thumbnailSize = (std::max)(64.0f, m_thumbnailSize);
	const ImVec2 cardSize(cellWidth - 8.0f, thumbnailSize + 42.0f);

	if (displayItems.empty())
	{
		if (!m_visibleThumbnailItemsKnown || !m_visibleThumbnailItems.empty())
		{
			m_visibleThumbnailItems.clear();
			m_visibleThumbnailItemsKnown = true;
			m_thumbnailGenerationScopeDirty = true;
		}
		UpdateThumbnailGenerationScope();
		ImGui::TextDisabled("This folder is empty");
		return;
	}

	const auto itemCount = static_cast<int>(displayItems.size());
	const auto rowCount = (itemCount + columns - 1) / columns;
	std::vector<NLS::Editor::Assets::AssetBrowserItem> visibleThumbnailItems;
	ImGuiListClipper clipper;
	clipper.Begin(rowCount, cardSize.y + ImGui::GetStyle().ItemSpacing.y);
	while (clipper.Step())
	{
		for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
		{
			ImGui::Columns(columns, "##AssetBrowserGrid", false);
			const auto rowPresentationGeneration = m_projectAssetPresentationGeneration;
			for (int column = 0; column < columns; ++column)
			{
				if (rowPresentationGeneration != m_projectAssetPresentationGeneration)
				{
					ImGui::Columns(1);
					return;
				}
				const auto presentationGeneration = m_projectAssetPresentationGeneration;
				const auto itemIndex = row * columns + column;
				if (itemIndex >= itemCount)
				{
					ImGui::NextColumn();
					continue;
				}

				const auto& displayItem = displayItems[static_cast<size_t>(itemIndex)];
				const auto& item = displayItem.item;
				visibleThumbnailItems.push_back(item);
				ImGui::PushID(item.projectRelativePath.c_str());

				const bool selected = m_selectedProjectItem == item.projectRelativePath;
				const ImVec2 cursor = ImGui::GetCursorScreenPos();
				if (selected)
				{
					ImGui::GetWindowDrawList()->AddRectFilled(
						cursor,
						ImVec2(cursor.x + cardSize.x, cursor.y + cardSize.y),
						ImGui::GetColorU32(ImGuiCol_Header),
						4.0f);
				}

				const float childIndent = displayItem.subAsset ? 14.0f : 0.0f;
				const float visibleThumbnailSize = displayItem.subAsset
					? (std::max)(48.0f, thumbnailSize - 18.0f)
					: thumbnailSize;
				const ImVec2 iconMin(
					cursor.x + childIndent + (cardSize.x - childIndent - visibleThumbnailSize) * 0.5f,
					cursor.y + 4.0f + (thumbnailSize - visibleThumbnailSize) * 0.5f);
				const ImVec2 iconMax(iconMin.x + visibleThumbnailSize, iconMin.y + visibleThumbnailSize);
				ImGui::InvisibleButton("##assetCard", cardSize);
				const bool hovered = ImGui::IsItemHovered();
				if (hovered && !selected)
				{
					ImGui::GetWindowDrawList()->AddRectFilled(
						cursor,
						ImVec2(cursor.x + cardSize.x, cursor.y + cardSize.y),
						ImGui::GetColorU32(ImGuiCol_HeaderHovered),
						4.0f);
				}
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
				{
					if (displayItem.childCount > 0u)
					{
						const auto sourcePath = item.sourceAssetPath.empty()
							? item.projectRelativePath
							: item.sourceAssetPath;
						if (displayItem.expanded)
							m_expandedProjectAssetItems.erase(sourcePath);
						else
							m_expandedProjectAssetItems.insert(sourcePath);
					}
					SelectProjectGridItem(item);
				}
				DrawProjectGridItemDragSource(item);
				DrawProjectGridItemContextMenu(item);
				if (presentationGeneration != m_projectAssetPresentationGeneration)
				{
					ImGui::PopID();
					ImGui::Columns(1);
					return;
				}
				if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
					DrawProjectFolderDropTarget(item.projectRelativePath, item.absolutePath);
				if (presentationGeneration != m_projectAssetPresentationGeneration)
				{
					ImGui::PopID();
					ImGui::Columns(1);
					return;
				}
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					const bool openingWillInvalidateGrid =
						NLS::Editor::Assets::ShouldStopDrawingAssetBrowserGridAfterOpeningItem(
							m_selectedProjectFolder,
							item);
					OpenProjectGridItem(item);
					if (openingWillInvalidateGrid ||
						presentationGeneration != m_projectAssetPresentationGeneration)
					{
						ImGui::PopID();
						ImGui::Columns(1);
						return;
					}
				}

				auto* drawList = ImGui::GetWindowDrawList();
				if (displayItem.subAsset)
				{
					drawList->AddLine(
						ImVec2(cursor.x + 8.0f, cursor.y + 2.0f),
						ImVec2(cursor.x + 8.0f, cursor.y + cardSize.y - 20.0f),
						ImGui::GetColorU32(ImGuiCol_Border));
					drawList->AddLine(
						ImVec2(cursor.x + 8.0f, iconMin.y + visibleThumbnailSize * 0.5f),
						ImVec2(iconMin.x - 4.0f, iconMin.y + visibleThumbnailSize * 0.5f),
						ImGui::GetColorU32(ImGuiCol_Border));
				}
				DrawProjectGridItemThumbnail(item, iconMin, iconMax, visibleThumbnailSize, hovered);
				if (displayItem.childCount > 0u)
				{
					const float stripHeight = 14.0f;
					const ImVec2 stripMin(iconMin.x, iconMax.y - stripHeight);
					const ImVec2 stripMax(iconMax.x, iconMax.y);
					drawList->AddRectFilled(
						stripMin,
						stripMax,
						IM_COL32(26, 30, 34, hovered ? 220 : 190),
						0.0f);
					const char* marker = displayItem.expanded ? "v" : ">";
					drawList->AddText(
						ImVec2(stripMin.x + 5.0f, stripMin.y),
						ImGui::GetColorU32(ImGuiCol_Text),
						marker);
					const auto subAssetLabel = EllipsizeAssetBrowserLabel(
						"Sub-assets",
						iconMax.x - stripMin.x - 22.0f);
					drawList->AddText(
						ImVec2(stripMin.x + 18.0f, stripMin.y),
						ImGui::GetColorU32(ImGuiCol_TextDisabled),
						subAssetLabel.c_str());
				}

				const auto label = EllipsizeAssetBrowserLabel(item.displayName, cardSize.x - 8.0f);
				const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
				const float labelX = cursor.x + (std::max)(0.0f, (cardSize.x - labelSize.x) * 0.5f);
				drawList->AddText(
					ImVec2(labelX, iconMax.y + 8.0f),
					ImGui::GetColorU32(ImGuiCol_Text),
					label.c_str());

				if (hovered)
					ImGui::SetTooltip("%s", item.projectRelativePath.c_str());

				ImGui::NextColumn();
				ImGui::PopID();
			}
			ImGui::Columns(1);
		}
	}
	const auto visibleScopeKey = NLS::Editor::Assets::BuildAssetBrowserThumbnailGenerationScopeKey(
		NormalizeProjectBrowserPath(m_selectedProjectFolder),
		AssetBrowserThumbnailRequestSize(m_thumbnailSize),
		visibleThumbnailItems);
	const auto previousVisibleScopeKey = NLS::Editor::Assets::BuildAssetBrowserThumbnailGenerationScopeKey(
		NormalizeProjectBrowserPath(m_selectedProjectFolder),
		AssetBrowserThumbnailRequestSize(m_thumbnailSize),
		m_visibleThumbnailItems);
	if (!m_visibleThumbnailItemsKnown || visibleScopeKey != previousVisibleScopeKey)
	{
		m_visibleThumbnailItems = std::move(visibleThumbnailItems);
		m_visibleThumbnailItemsKnown = true;
		m_thumbnailGenerationScopeDirty = true;
	}
	UpdateThumbnailGenerationScope();

	if (const auto generated = m_thumbnailService.ConsumeCompletedThumbnail();
		generated.has_value())
	{
		if (generated->status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh &&
			!generated->imagePath.empty())
		{
			const auto key = generated->imagePath.lexically_normal().generic_string();
			ReleaseCachedThumbnailTexture(key);
			m_thumbnailTexturesFailedToLoad.erase(key);
			m_thumbnailTexturesQueuedForLoad.erase(key);
			m_thumbnailTextureLoadQueue.erase(
				std::remove(m_thumbnailTextureLoadQueue.begin(), m_thumbnailTextureLoadQueue.end(), key),
				m_thumbnailTextureLoadQueue.end());
		}
		if (generated->cacheEntry.has_value())
		{
			if (const auto foundItemKey = m_thumbnailItemKeyByCacheKey.find(generated->cacheEntry->cacheKey);
				foundItemKey != m_thumbnailItemKeyByCacheKey.end())
			{
				m_thumbnailResultsByItemKey[foundItemKey->second] = *generated;
			}
		}
	}
	if (m_thumbnailService.GetQueuedRequestCount() > 0u)
	{
		(void)m_thumbnailService.StartNextThumbnailGeneration();
	}
}

void Editor::Panels::AssetBrowser::DrawCurrentFolderList(
	const std::vector<NLS::Editor::Assets::AssetBrowserDisplayItem>& displayItems)
{
	if (displayItems.empty())
	{
		if (!m_visibleThumbnailItemsKnown || !m_visibleThumbnailItems.empty())
		{
			m_visibleThumbnailItems.clear();
			m_visibleThumbnailItemsKnown = true;
			m_thumbnailGenerationScopeDirty = true;
		}
		UpdateThumbnailGenerationScope();
		ImGui::TextDisabled("This folder is empty");
		return;
	}

	std::vector<NLS::Editor::Assets::AssetBrowserItem> visibleThumbnailItems;
	const float rowHeight = 26.0f;
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(displayItems.size()), rowHeight);
	while (clipper.Step())
	{
		for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
		{
			const auto& displayItem = displayItems[static_cast<size_t>(index)];
			const auto& item = displayItem.item;
			const auto presentationGeneration = m_projectAssetPresentationGeneration;
			visibleThumbnailItems.push_back(item);

			ImGui::PushID(item.projectRelativePath.c_str());
			const ImVec2 cursor = ImGui::GetCursorScreenPos();
			const float indent = displayItem.subAsset ? 20.0f : 0.0f;
			const ImVec2 rowSize(ImGui::GetContentRegionAvail().x, rowHeight);
			const bool selected = m_selectedProjectItem == item.projectRelativePath;
			ImGui::InvisibleButton("##assetListRow", rowSize);
			const bool hovered = ImGui::IsItemHovered();
			if (selected || hovered)
			{
				ImGui::GetWindowDrawList()->AddRectFilled(
					cursor,
					ImVec2(cursor.x + rowSize.x, cursor.y + rowSize.y),
					ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered),
					2.0f);
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			{
				if (displayItem.childCount > 0u)
				{
					const auto sourcePath = item.sourceAssetPath.empty()
						? item.projectRelativePath
						: item.sourceAssetPath;
					if (displayItem.expanded)
						m_expandedProjectAssetItems.erase(sourcePath);
					else
						m_expandedProjectAssetItems.insert(sourcePath);
				}
				SelectProjectGridItem(item);
			}

			if (displayItem.childCount > 0u)
			{
				ImGui::GetWindowDrawList()->AddText(
					ImVec2(cursor.x + 4.0f, cursor.y + 5.0f),
					ImGui::GetColorU32(ImGuiCol_Text),
					displayItem.expanded ? "v" : ">");
			}
			if (displayItem.subAsset)
			{
				ImGui::GetWindowDrawList()->AddText(
					ImVec2(cursor.x + 6.0f, cursor.y + 5.0f),
					ImGui::GetColorU32(ImGuiCol_TextDisabled),
					"-");
			}

			const ImVec2 iconMin(cursor.x + 18.0f + indent, cursor.y + 4.0f);
			const ImVec2 iconMax(iconMin.x + 18.0f, iconMin.y + 18.0f);
			DrawProjectGridItemThumbnail(item, iconMin, iconMax, 18.0f, hovered, true);

			const float textX = iconMax.x + 8.0f;
			ImGui::GetWindowDrawList()->AddText(
				ImVec2(textX, cursor.y + 5.0f),
				ImGui::GetColorU32(ImGuiCol_Text),
				item.displayName.c_str());
			const char* typeLabel = NLS::Editor::Assets::AssetBrowserItemTypeDisplayLabel(item.type);
			const ImVec2 typeSize = ImGui::CalcTextSize(typeLabel);
			ImGui::GetWindowDrawList()->AddText(
				ImVec2(cursor.x + rowSize.x - typeSize.x - 12.0f, cursor.y + 5.0f),
				ImGui::GetColorU32(ImGuiCol_TextDisabled),
				typeLabel);

			DrawProjectGridItemDragSource(item);
			DrawProjectGridItemContextMenu(item);
			if (presentationGeneration != m_projectAssetPresentationGeneration)
			{
				ImGui::PopID();
				return;
			}
			if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
				DrawProjectFolderDropTarget(item.projectRelativePath, item.absolutePath);
			if (presentationGeneration != m_projectAssetPresentationGeneration)
			{
				ImGui::PopID();
				return;
			}
			if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				OpenProjectGridItem(item);
				if (presentationGeneration != m_projectAssetPresentationGeneration)
				{
					ImGui::PopID();
					return;
				}
			}
			if (hovered)
				ImGui::SetTooltip("%s", item.projectRelativePath.c_str());
			ImGui::PopID();
		}
	}

	const auto visibleScopeKey = NLS::Editor::Assets::BuildAssetBrowserThumbnailGenerationScopeKey(
		NormalizeProjectBrowserPath(m_selectedProjectFolder),
		AssetBrowserThumbnailRequestSize(m_thumbnailSize),
		visibleThumbnailItems);
	const auto previousVisibleScopeKey = NLS::Editor::Assets::BuildAssetBrowserThumbnailGenerationScopeKey(
		NormalizeProjectBrowserPath(m_selectedProjectFolder),
		AssetBrowserThumbnailRequestSize(m_thumbnailSize),
		m_visibleThumbnailItems);
	if (!m_visibleThumbnailItemsKnown || visibleScopeKey != previousVisibleScopeKey)
	{
		m_visibleThumbnailItems = std::move(visibleThumbnailItems);
		m_visibleThumbnailItemsKnown = true;
		m_thumbnailGenerationScopeDirty = true;
	}
	UpdateThumbnailGenerationScope();
	if (const auto generated = m_thumbnailService.ConsumeCompletedThumbnail();
		generated.has_value())
	{
		if (generated->status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh &&
			!generated->imagePath.empty())
		{
			const auto key = generated->imagePath.lexically_normal().generic_string();
			ReleaseCachedThumbnailTexture(key);
			m_thumbnailTexturesFailedToLoad.erase(key);
			m_thumbnailTexturesQueuedForLoad.erase(key);
			m_thumbnailTextureLoadQueue.erase(
				std::remove(m_thumbnailTextureLoadQueue.begin(), m_thumbnailTextureLoadQueue.end(), key),
				m_thumbnailTextureLoadQueue.end());
		}
		if (generated->cacheEntry.has_value())
		{
			if (const auto foundItemKey = m_thumbnailItemKeyByCacheKey.find(generated->cacheEntry->cacheKey);
				foundItemKey != m_thumbnailItemKeyByCacheKey.end())
			{
				m_thumbnailResultsByItemKey[foundItemKey->second] = *generated;
			}
		}
	}
	if (m_thumbnailService.GetQueuedRequestCount() > 0u)
	{
		(void)m_thumbnailService.StartNextThumbnailGeneration();
	}
}

Editor::Panels::AssetBrowser::ThumbnailTextureHandle Editor::Panels::AssetBrowser::ResolveCachedThumbnailTextureHandle(
	const std::filesystem::path& imagePath)
{
	if (imagePath.empty())
		return {};

	const auto normalizedPath = imagePath.lexically_normal().generic_string();
	if (const auto found = m_thumbnailTexturesByPath.find(normalizedPath); found != m_thumbnailTexturesByPath.end())
	{
		m_thumbnailTexturesUsedThisFrame.insert(normalizedPath);
		m_thumbnailTextureLru.erase(
			std::remove(m_thumbnailTextureLru.begin(), m_thumbnailTextureLru.end(), normalizedPath),
			m_thumbnailTextureLru.end());
		m_thumbnailTextureLru.push_back(normalizedPath);
		if (found->second.textureView != nullptr &&
			NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
			{
				return {
					NLS_SERVICE(NLS::UI::UIManager).ResolveTextureId(found->second.textureView),
					found->second.width,
					found->second.height
				};
			}
			ReleaseCachedThumbnailTexture(normalizedPath);
			return {};
		}

		QueueCachedThumbnailTextureLoad(imagePath);
		return {};
	}

bool Editor::Panels::AssetBrowser::LoadCachedThumbnailTexture(
	const std::string& normalizedPath)
{
	return LoadDecodedCachedThumbnailTexture(DecodeCachedThumbnailTexture(normalizedPath));
}

bool Editor::Panels::AssetBrowser::LoadDecodedCachedThumbnailTexture(
	ThumbnailTextureDecodeResult result)
{
	const auto normalizedPath = std::move(result.normalizedPath);
	if (normalizedPath.empty() ||
		result.rgbaPixels.empty() ||
		result.width == 0u ||
		result.height == 0u ||
		!NLS::Editor::Assets::IsAssetBrowserCachedThumbnailTextureSizeAllowed(
			result.width,
			result.height,
			kMaxAssetBrowserCachedThumbnailTextureDimension) ||
		m_thumbnailTexturesByPath.find(normalizedPath) != m_thumbnailTexturesByPath.end() ||
		m_thumbnailTexturesFailedToLoad.find(normalizedPath) != m_thumbnailTexturesFailedToLoad.end())
	{
		if (!normalizedPath.empty() && result.rgbaPixels.empty())
		{
			InvalidateAssetThumbnailMetadataForImagePath(normalizedPath);
			m_thumbnailTexturesFailedToLoad.insert(normalizedPath);
		}
		return false;
	}

	auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreateFromMemory(
		result.rgbaPixels.data(),
		result.width,
		result.height,
		NLS::Render::Settings::ETextureFilteringMode::LINEAR,
		NLS::Render::Settings::ETextureFilteringMode::LINEAR,
		false);
	if (texture == nullptr)
	{
		InvalidateAssetThumbnailMetadataForImagePath(normalizedPath);
		m_thumbnailTexturesFailedToLoad.insert(normalizedPath);
		return false;
	}

	const auto textureView = texture->GetOrCreateExplicitTextureView("AssetBrowser.Thumbnail");
	if (textureView == nullptr || !NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
	{
		NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture);
		InvalidateAssetThumbnailMetadataForImagePath(normalizedPath);
		m_thumbnailTexturesFailedToLoad.insert(normalizedPath);
		return false;
	}

	if (NLS_SERVICE(NLS::UI::UIManager).ResolveTextureId(textureView) == nullptr)
	{
		NLS_SERVICE(NLS::UI::UIManager).ReleaseTextureViewHandle(textureView);
		NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture);
		InvalidateAssetThumbnailMetadataForImagePath(normalizedPath);
		m_thumbnailTexturesFailedToLoad.insert(normalizedPath);
		return false;
	}

	m_thumbnailTexturesByPath.emplace(normalizedPath, ThumbnailTextureCacheEntry {
		texture,
		textureView,
		result.width,
		result.height
	});
	m_thumbnailTextureLru.push_back(normalizedPath);
	return true;
}

Editor::Panels::AssetBrowser::ThumbnailTextureDecodeResult
Editor::Panels::AssetBrowser::DecodeCachedThumbnailTexture(
	std::string normalizedPath)
{
	ThumbnailTextureDecodeResult result;
	result.normalizedPath = std::move(normalizedPath);
	if (result.normalizedPath.empty())
		return result;

	try
	{
		NLS::Image image(result.normalizedPath, true);
		const auto* source = image.GetData();
		const auto width = image.GetWidth();
		const auto height = image.GetHeight();
		const auto channels = image.GetChannels();
		if (source == nullptr || width <= 0 || height <= 0 || channels <= 0 || channels > 4)
			return result;

		result.width = static_cast<uint32_t>(width);
		result.height = static_cast<uint32_t>(height);
		if (!NLS::Editor::Assets::IsAssetBrowserCachedThumbnailTextureSizeAllowed(
				result.width,
				result.height,
				kMaxAssetBrowserCachedThumbnailTextureDimension))
		{
			result.width = 0u;
			result.height = 0u;
			return result;
		}

		const auto pixelCount = static_cast<size_t>(result.width) * static_cast<size_t>(result.height);
		result.rgbaPixels.resize(pixelCount * 4u, 255u);
		for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
		{
			const auto sourceIndex = pixel * static_cast<size_t>(channels);
			const auto targetIndex = pixel * 4u;
			switch (channels)
			{
			case 1:
				result.rgbaPixels[targetIndex + 0u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 1u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 2u] = source[sourceIndex + 0u];
				break;
			case 2:
				result.rgbaPixels[targetIndex + 0u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 1u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 2u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 3u] = source[sourceIndex + 1u];
				break;
			case 3:
				result.rgbaPixels[targetIndex + 0u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 1u] = source[sourceIndex + 1u];
				result.rgbaPixels[targetIndex + 2u] = source[sourceIndex + 2u];
				break;
			case 4:
				result.rgbaPixels[targetIndex + 0u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 1u] = source[sourceIndex + 1u];
				result.rgbaPixels[targetIndex + 2u] = source[sourceIndex + 2u];
				result.rgbaPixels[targetIndex + 3u] = source[sourceIndex + 3u];
				break;
			default:
				result.rgbaPixels.clear();
				break;
			}
		}
	}
	catch (const std::bad_alloc&)
	{
		result.rgbaPixels.clear();
		result.width = 0u;
		result.height = 0u;
	}
	catch (...)
	{
		result.rgbaPixels.clear();
		result.width = 0u;
		result.height = 0u;
	}
	return result;
}

void Editor::Panels::AssetBrowser::QueueCachedThumbnailTextureLoad(
	const std::filesystem::path& imagePath)
{
	if (imagePath.empty())
		return;

	const auto normalizedPath = imagePath.lexically_normal().generic_string();
	if (normalizedPath.empty() ||
		m_thumbnailTexturesByPath.find(normalizedPath) != m_thumbnailTexturesByPath.end() ||
		m_thumbnailTexturesQueuedForLoad.find(normalizedPath) != m_thumbnailTexturesQueuedForLoad.end() ||
		m_thumbnailTexturesFailedToLoad.find(normalizedPath) != m_thumbnailTexturesFailedToLoad.end())
	{
		return;
	}

	m_thumbnailTextureLoadQueue.push_back(normalizedPath);
	m_thumbnailTexturesQueuedForLoad.insert(normalizedPath);
}

void Editor::Panels::AssetBrowser::PumpQueuedCachedThumbnailTextureLoads()
{
	ConsumeCompletedCachedThumbnailTextureDecodes();
	StartQueuedCachedThumbnailTextureDecodes();
}

void Editor::Panels::AssetBrowser::StartQueuedCachedThumbnailTextureDecodes()
{
	const auto startBudget = NLS::Editor::Assets::AssetBrowserThumbnailTextureDecodeStartBudget(
		m_thumbnailTextureDecodes.size(),
		kMaxAssetBrowserThumbnailTextureDecodesInFlight);
	if (startBudget == 0u)
		return;

	std::unordered_set<std::string> residentKeys;
	residentKeys.reserve(m_thumbnailTexturesByPath.size());
	for (const auto& [key, _] : m_thumbnailTexturesByPath)
		residentKeys.insert(key);

	const auto candidates = NLS::Editor::Assets::SelectAssetBrowserThumbnailTextureDecodeCandidates(
		m_thumbnailTextureLoadQueue,
		residentKeys,
		m_thumbnailTexturesDecoding,
		std::min(kMaxAssetBrowserThumbnailTextureLoadsPerFrame, startBudget));
	for (const auto& key : candidates)
	{
		m_thumbnailTexturesDecoding.insert(key);
		try
		{
			m_thumbnailTextureDecodes.push_back({
				key,
				std::async(
					std::launch::async,
					[key]
					{
						return DecodeCachedThumbnailTexture(key);
					})
			});
		}
		catch (...)
		{
			m_thumbnailTexturesDecoding.erase(key);
			m_thumbnailTexturesQueuedForLoad.erase(key);
			m_thumbnailTexturesFailedToLoad.insert(key);
		}
	}

	m_thumbnailTextureLoadQueue.erase(
		std::remove_if(
			m_thumbnailTextureLoadQueue.begin(),
			m_thumbnailTextureLoadQueue.end(),
			[this](const std::string& key)
			{
				return m_thumbnailTexturesQueuedForLoad.find(key) == m_thumbnailTexturesQueuedForLoad.end() ||
					m_thumbnailTexturesByPath.find(key) != m_thumbnailTexturesByPath.end() ||
					m_thumbnailTexturesFailedToLoad.find(key) != m_thumbnailTexturesFailedToLoad.end();
			}),
		m_thumbnailTextureLoadQueue.end());
}

void Editor::Panels::AssetBrowser::ConsumeCompletedCachedThumbnailTextureDecodes()
{
	for (auto iterator = m_thumbnailTextureDecodes.begin(); iterator != m_thumbnailTextureDecodes.end();)
	{
		if (!iterator->future.valid())
		{
			m_thumbnailTexturesDecoding.erase(iterator->normalizedPath);
			iterator = m_thumbnailTextureDecodes.erase(iterator);
			continue;
		}

		if (iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			++iterator;
			continue;
		}

		ThumbnailTextureDecodeResult result;
		try
		{
			result = iterator->future.get();
		}
		catch (const std::bad_alloc&)
		{
			result.normalizedPath = iterator->normalizedPath;
		}
		catch (...)
		{
			result.normalizedPath = iterator->normalizedPath;
		}
		const auto normalizedPath = result.normalizedPath;
		(void)LoadDecodedCachedThumbnailTexture(std::move(result));
		m_thumbnailTexturesQueuedForLoad.erase(normalizedPath);
		m_thumbnailTexturesDecoding.erase(normalizedPath);
		iterator = m_thumbnailTextureDecodes.erase(iterator);
	}
}

void Editor::Panels::AssetBrowser::RequestObjectReferencePickerEntriesRefresh()
{
	NLS::Editor::Assets::MarkObjectReferencePickerEntriesDirty();
	++m_objectReferencePickerRefreshGeneration;
	m_objectReferencePickerRefreshRequested = true;
}

void Editor::Panels::AssetBrowser::PumpRetiredProjectAssetDatabaseRefreshes()
{
	for (auto iterator = m_retiredProjectAssetDatabaseRefreshes.begin();
		iterator != m_retiredProjectAssetDatabaseRefreshes.end();)
	{
		if (!iterator->future.valid())
		{
			iterator = m_retiredProjectAssetDatabaseRefreshes.erase(iterator);
			continue;
		}
		if (iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			++iterator;
			continue;
		}
		try
		{
			(void)iterator->future.get();
		}
		catch (...)
		{
		}
		iterator = m_retiredProjectAssetDatabaseRefreshes.erase(iterator);
	}
}

void Editor::Panels::AssetBrowser::DiscardProjectAssetDatabaseRefresh()
{
	if (m_projectAssetDatabaseRefresh.has_value() &&
		m_projectAssetDatabaseRefresh->future.valid())
	{
		const bool ready =
			m_projectAssetDatabaseRefresh->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
		if (NLS::Editor::Assets::PlanAssetDatabaseRefreshDiscardAction(true, ready) ==
			NLS::Editor::Assets::AssetDatabaseRefreshDiscardAction::Retire)
		{
			m_retiredProjectAssetDatabaseRefreshes.push_back(std::move(*m_projectAssetDatabaseRefresh));
			m_projectAssetDatabaseRefresh.reset();
			return;
		}
	}
	m_projectAssetDatabaseRefresh.reset();
}

void Editor::Panels::AssetBrowser::DiscardObjectReferencePickerEntriesRefresh()
{
	if (m_objectReferencePickerRefresh.has_value() &&
		m_objectReferencePickerRefresh->future.valid())
	{
		m_retiredObjectReferencePickerRefreshes.push_back(std::move(*m_objectReferencePickerRefresh));
	}
	m_objectReferencePickerRefresh.reset();
}

void Editor::Panels::AssetBrowser::PumpObjectReferencePickerEntriesRefresh()
{
	for (auto iterator = m_retiredObjectReferencePickerRefreshes.begin();
		iterator != m_retiredObjectReferencePickerRefreshes.end();)
	{
		if (!iterator->future.valid())
		{
			iterator = m_retiredObjectReferencePickerRefreshes.erase(iterator);
			continue;
		}
		if (iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			++iterator;
			continue;
		}
		try
		{
			(void)iterator->future.get();
		}
		catch (...)
		{
		}
		iterator = m_retiredObjectReferencePickerRefreshes.erase(iterator);
	}

	if (m_objectReferencePickerRefresh.has_value())
	{
		auto& refresh = *m_objectReferencePickerRefresh;
		if (!refresh.future.valid())
		{
			m_objectReferencePickerRefresh.reset();
		}
		else if (refresh.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			std::vector<NLS::Editor::Assets::ObjectReferencePickerEntry> entries;
			bool refreshSucceeded = true;
			try
			{
				entries = refresh.future.get();
			}
			catch (...)
			{
				refreshSucceeded = false;
			}
			const bool currentGeneration = refresh.generation == m_objectReferencePickerRefreshGeneration;
			m_objectReferencePickerRefresh.reset();
			if (currentGeneration && refreshSucceeded)
			{
				NLS::Editor::Assets::SetObjectReferencePickerEntries(std::move(entries));
				m_objectReferencePickerRefreshRequested = false;
			}
			else if (currentGeneration)
			{
				NLS::Editor::Assets::MarkObjectReferencePickerEntriesDirty();
				m_objectReferencePickerRefreshRequested = true;
			}
		}
	}

	if (m_objectReferencePickerRefresh.has_value() ||
		!m_objectReferencePickerRefreshRequested ||
		!m_projectAssetDatabaseReady ||
		!m_projectAssetDatabase ||
		!NLS::Editor::Assets::ShouldDeferObjectReferencePickerEntriesRefresh())
	{
		return;
	}

	const auto generation = m_objectReferencePickerRefreshGeneration;
	auto snapshots = m_projectAssetDatabase->GetObjectReferencePickerAssetSnapshots();
	try
	{
		m_objectReferencePickerRefresh = ObjectReferencePickerRefresh {
			generation,
			std::async(
				std::launch::async,
				[snapshots = std::move(snapshots)]() mutable
				{
					return NLS::Editor::Assets::BuildObjectReferencePickerEntriesFromSnapshots(snapshots);
				})
		};
	}
	catch (...)
	{
		NLS::Editor::Assets::MarkObjectReferencePickerEntriesDirty();
		m_objectReferencePickerRefreshRequested = true;
	}
}

void Editor::Panels::AssetBrowser::DestroyCachedThumbnailTextures(const bool force)
{
	for (auto iterator = m_thumbnailTextureDecodes.begin(); iterator != m_thumbnailTextureDecodes.end();)
	{
		if (!iterator->future.valid())
		{
			m_thumbnailTexturesDecoding.erase(iterator->normalizedPath);
			iterator = m_thumbnailTextureDecodes.erase(iterator);
			continue;
		}
		if (!force && iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			++iterator;
			continue;
		}
		try
		{
			(void)iterator->future.get();
		}
		catch (...)
		{
		}
		m_thumbnailTexturesDecoding.erase(iterator->normalizedPath);
		iterator = m_thumbnailTextureDecodes.erase(iterator);
	}

	std::vector<std::string> textureKeys;
	textureKeys.reserve(m_thumbnailTexturesByPath.size());
	for (const auto& [key, _] : m_thumbnailTexturesByPath)
		textureKeys.push_back(key);
	if (!force)
	{
		const auto clearPlan = NLS::Editor::Assets::PlanAssetBrowserThumbnailTextureFullClear(
			textureKeys,
			m_thumbnailTexturesUsedThisFrame,
			m_thumbnailTexturesPendingRelease);
		m_thumbnailTexturesUsedThisFrame = clearPlan.usedThisFrame;
		m_thumbnailTexturesPendingRelease = clearPlan.pendingRelease;
		textureKeys = clearPlan.releaseNow;
	}
	for (const auto& key : textureKeys)
	{
		if (!force)
		{
			const auto found = m_thumbnailTexturesByPath.find(key);
			if (found == m_thumbnailTexturesByPath.end())
				continue;
			if (found->second.textureView != nullptr &&
				NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
			{
				NLS_SERVICE(NLS::UI::UIManager).RetireTextureViewHandle(found->second.textureView);
			}
			NLS::Render::Resources::Loaders::TextureLoader::Destroy(found->second.texture);
			m_thumbnailTexturesByPath.erase(found);
			continue;
		}

		const auto found = m_thumbnailTexturesByPath.find(key);
		if (found == m_thumbnailTexturesByPath.end())
			continue;
		if (found->second.textureView != nullptr &&
			NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
		{
			NLS_SERVICE(NLS::UI::UIManager).ReleaseTextureViewHandle(found->second.textureView);
		}
		NLS::Render::Resources::Loaders::TextureLoader::Destroy(found->second.texture);
		m_thumbnailTexturesByPath.erase(found);
	}
	m_thumbnailTextureLru.clear();
	if (force)
	{
		m_thumbnailTexturesUsedThisFrame.clear();
		m_thumbnailTexturesPendingRelease.clear();
	}
	m_thumbnailTextureLoadQueue.clear();
	m_thumbnailTexturesQueuedForLoad.clear();
	m_thumbnailTexturesFailedToLoad.clear();
	if (force)
		m_thumbnailTexturesDecoding.clear();
}

void Editor::Panels::AssetBrowser::ReleaseCachedThumbnailTexture(
	const std::string& normalizedPath)
{
	const auto found = m_thumbnailTexturesByPath.find(normalizedPath);
	if (found == m_thumbnailTexturesByPath.end())
		return;
	if (m_thumbnailTexturesUsedThisFrame.find(normalizedPath) != m_thumbnailTexturesUsedThisFrame.end())
	{
		m_thumbnailTexturesPendingRelease.insert(normalizedPath);
		return;
	}

	if (found->second.textureView != nullptr &&
		NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
	{
		NLS_SERVICE(NLS::UI::UIManager).RetireTextureViewHandle(found->second.textureView);
	}
	NLS::Render::Resources::Loaders::TextureLoader::Destroy(found->second.texture);
	m_thumbnailTexturesByPath.erase(found);
	m_thumbnailTextureLru.erase(
		std::remove(m_thumbnailTextureLru.begin(), m_thumbnailTextureLru.end(), normalizedPath),
		m_thumbnailTextureLru.end());
}

void Editor::Panels::AssetBrowser::PruneCachedThumbnailTextures()
{
	const auto evictions = NLS::Editor::Assets::SelectAssetBrowserThumbnailTextureEvictionCandidates(
		m_thumbnailTextureLru,
		m_thumbnailTexturesUsedThisFrame,
		kMaxResidentAssetBrowserThumbnailTextures);
	for (const auto& key : evictions)
		ReleaseCachedThumbnailTexture(key);
}

void Editor::Panels::AssetBrowser::UpdateThumbnailGenerationScope()
{
	const auto thumbnailItems = NLS::Editor::Assets::SelectAssetBrowserThumbnailGenerationItems(
		m_currentFolderItems,
		m_visibleThumbnailItems,
		m_visibleThumbnailItemsKnown);
	const auto nextSize = AssetBrowserThumbnailRequestSize(m_thumbnailSize);
	const auto nextFolder = NormalizeProjectBrowserPath(m_selectedProjectFolder);
	const auto nextScopeKey = NLS::Editor::Assets::BuildAssetBrowserThumbnailGenerationScopeKey(
		nextFolder,
		nextSize,
		thumbnailItems);
	const auto decision = NLS::Editor::Assets::EvaluateAssetBrowserThumbnailGenerationScope(
		m_lastThumbnailGenerationScopeKey,
		m_lastThumbnailRequestSize,
		m_thumbnailGenerationScopeDirty,
		nextScopeKey,
		nextSize);
	if (decision.canSkip)
	{
		return;
	}

	m_lastThumbnailRequestSize = nextSize;
	m_lastThumbnailGenerationScopeKey = nextScopeKey;
	m_thumbnailGenerationScopeDirty = false;
	if (decision.scopeChanged)
		m_thumbnailService.SupersedeQueuedRequestsForGeneration(nextScopeKey);
	else
		m_thumbnailService.ClearQueuedRequests();
	m_thumbnailResultsByItemKey.clear();
	m_thumbnailItemKeyByCacheKey.clear();

	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	for (const auto& item : thumbnailItems)
	{
		if (const auto request = NLS::Editor::Assets::BuildAssetThumbnailRequestForItem(
				projectRoot,
				item,
				nextSize))
		{
			const auto itemThumbnailKey = item.projectRelativePath + "#" + std::to_string(nextSize);
			const auto thumbnail = m_thumbnailService.GetThumbnail(*request);
			m_thumbnailResultsByItemKey.emplace(itemThumbnailKey, thumbnail);
			if (thumbnail.cacheEntry.has_value())
				m_thumbnailItemKeyByCacheKey[thumbnail.cacheEntry->cacheKey] = itemThumbnailKey;
		}
	}
}

NLS::Editor::Assets::AssetBrowserRect MakeAssetBrowserRect(
    const ImVec2& min,
    const ImVec2& max)
{
    return {
        { min.x, min.y },
        { max.x, max.y }
    };
}

ImVec2 ToImVec2(const NLS::Editor::Assets::AssetBrowserPoint& point)
{
    return { point.x, point.y };
}

void Editor::Panels::AssetBrowser::DrawProjectGridItemThumbnail(
	const NLS::Editor::Assets::AssetBrowserItem& item,
	const ImVec2& iconMin,
	const ImVec2& iconMax,
	const float thumbnailSize,
	const bool hovered,
	const bool compact)
{
	auto* drawList = ImGui::GetWindowDrawList();
	auto drawFallbackBlock = [&]()
	{
		const char* iconId = NLS::Editor::Assets::AssetBrowserFallbackIconId(item.type);
		if (void* textureHandle = ResolveAssetBrowserTextureHandle(
				EDITOR_CONTEXT(editorResources)->GetTexture(iconId),
				"AssetBrowser.TypeIcon"))
		{
			drawList->AddImage(textureHandle, iconMin, iconMax, kAssetBrowserImageUv0, kAssetBrowserImageUv1);
			return;
		}

		const auto color = AssetBrowserItemColor(item.type);
		drawList->AddRectFilled(iconMin, iconMax, color, compact ? 2.0f : 6.0f);
	};

	if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
	{
		if (void* textureHandle = ResolveAssetBrowserTextureHandle(
				EDITOR_CONTEXT(editorResources)->GetTexture("Icon_Folder"),
				"AssetBrowser.Folder"))
		{
			drawList->AddImage(textureHandle, iconMin, iconMax, kAssetBrowserImageUv0, kAssetBrowserImageUv1);
			return;
		}
		drawFallbackBlock();
		return;
	}

	const auto itemThumbnailKey =
		item.projectRelativePath + "#" + std::to_string(AssetBrowserThumbnailRequestSize(m_thumbnailSize));
	if (const auto thumbnailIterator = m_thumbnailResultsByItemKey.find(itemThumbnailKey);
		thumbnailIterator != m_thumbnailResultsByItemKey.end())
	{
		const auto& thumbnail = thumbnailIterator->second;
		if (thumbnail.status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh)
		{
			if (const auto textureInfo = ResolveCachedThumbnailTextureHandle(thumbnail.imagePath);
				textureInfo.textureHandle != nullptr)
			{
				const auto thumbnailRect = NLS::Editor::Assets::ComputeAssetBrowserThumbnailRect(
					MakeAssetBrowserRect(iconMin, iconMax),
					textureInfo.width,
					textureInfo.height);
				if (NLS::Editor::Assets::ShouldDrawAssetBrowserThumbnailLetterboxBackground(item.type))
					drawList->AddRectFilled(iconMin, iconMax, IM_COL32(0, 0, 0, 255), compact ? 2.0f : 6.0f);
				drawList->AddImage(
					textureInfo.textureHandle,
					ToImVec2(thumbnailRect.min),
					ToImVec2(thumbnailRect.max),
					kAssetBrowserImageUv0,
					kAssetBrowserImageUv1);
				return;
			}
		}

		if (!thumbnail.fallbackIcon.empty())
		{
				if (void* textureHandle = ResolveAssetBrowserTextureHandle(
						EDITOR_CONTEXT(editorResources)->GetTexture(thumbnail.fallbackIcon),
						"AssetBrowser.Fallback"))
				{
					drawList->AddImage(
						textureHandle,
						iconMin,
						iconMax,
						kAssetBrowserImageUv0,
						kAssetBrowserImageUv1);
					return;
				}
			}
	}

	drawFallbackBlock();
}

void Editor::Panels::AssetBrowser::DrawProjectGridItemDragSource(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
	{
		if (item.dragResourcePath.empty() ||
			!UI::BeginDragDropSource(
				UI::DragDropSourceFlags::NoDisableHover |
				UI::DragDropSourceFlags::NoHoldToOpenOthers))
		{
			return;
		}

		UI::DrawDragDropTooltipText(item.projectRelativePath.c_str());
		m_projectGridDragPairPayload = { item.projectRelativePath, nullptr };
		UI::SetDragDropPayload(
			"Folder",
			&m_projectGridDragPairPayload,
			sizeof(m_projectGridDragPairPayload));
		UI::EndDragDropSource();
		return;
	}

	const bool sourceEditorPayload =
		item.kind == NLS::Editor::Assets::AssetBrowserItemKind::SourceAsset &&
		(item.type == NLS::Editor::Assets::AssetBrowserItemType::Model ||
		 item.type == NLS::Editor::Assets::AssetBrowserItemType::Prefab ||
		 item.type == NLS::Editor::Assets::AssetBrowserItemType::Material ||
		 item.type == NLS::Editor::Assets::AssetBrowserItemType::Texture ||
		 item.type == NLS::Editor::Assets::AssetBrowserItemType::Shader);
	const bool generatedEditorPayload =
		item.kind == NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset &&
		!item.dragResourcePath.empty() &&
		item.assetId.IsValid() &&
		NLS::Editor::Assets::CanStoreEditorAssetDragPayload(
			item.dragResourcePath,
			item.assetId,
			item.subAssetKey);
	const bool filePayload =
		item.kind != NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset &&
		!item.dragResourcePath.empty();
	if (!sourceEditorPayload &&
		!generatedEditorPayload &&
		!filePayload)
	{
		return;
	}
	if (!UI::BeginDragDropSource(
			UI::DragDropSourceFlags::NoDisableHover |
			UI::DragDropSourceFlags::NoHoldToOpenOthers))
	{
		return;
	}

	auto editorAssetPayload = generatedEditorPayload
		? NLS::Editor::Assets::MakeAssetBrowserItemDragPayload(
			item,
			m_projectAssetDatabaseReady && m_projectAssetDatabase
				? m_projectAssetDatabase.get()
				: nullptr)
		: std::optional<NLS::Editor::Assets::EditorAssetDragPayload> {};
	if (!editorAssetPayload.has_value() &&
		sourceEditorPayload &&
		!item.absolutePath.empty() &&
		!item.dragResourcePath.empty())
	{
		editorAssetPayload = BuildEditorAssetDragPayloadForFile(
			m_projectAssetFolder,
			item.absolutePath.string(),
			item.dragResourcePath,
			Utils::PathParser::GetFileType(item.displayName));
	}

	UI::DrawDragDropTooltipText(item.displayName.c_str());
	if (editorAssetPayload.has_value())
	{
		UI::SetDragDropPayload(
			NLS::Editor::Assets::kEditorAssetDragPayloadType,
			&*editorAssetPayload,
			sizeof(*editorAssetPayload));
	}
	else if (filePayload)
	{
		m_projectGridDragPairPayload = { item.dragResourcePath, nullptr };
		UI::SetDragDropPayload(
			"File",
			&m_projectGridDragPairPayload,
			sizeof(m_projectGridDragPairPayload));
	}
	UI::EndDragDropSource();
}

void Editor::Panels::AssetBrowser::DrawProjectFolderDropTarget(
	const std::string& projectRelativeFolder,
	const std::filesystem::path& absoluteFolder)
{
	if (!UI::BeginDragDropTarget())
		return;

	if (const auto payload = UI::AcceptDragDropPayload(
			"Folder",
			UI::DragDropTargetFlags::None);
		payload.delivered &&
		payload.data != nullptr &&
		payload.dataSize == sizeof(std::pair<std::string, UI::Widgets::Group*>))
	{
		const auto* folderPayload = static_cast<const std::pair<std::string, UI::Widgets::Group*>*>(payload.data);
		(void)MoveOrCopyProjectBrowserFolderIntoFolder(
			folderPayload->first,
			absoluteFolder);
	}

	if (const auto payload = UI::AcceptDragDropPayload(
			"File",
			UI::DragDropTargetFlags::None);
		payload.delivered &&
		payload.data != nullptr &&
		payload.dataSize == sizeof(std::pair<std::string, UI::Widgets::Group*>))
	{
		const auto* filePayload = static_cast<const std::pair<std::string, UI::Widgets::Group*>*>(payload.data);
		(void)MoveOrCopyProjectBrowserFileIntoFolder(
			filePayload->first,
			absoluteFolder);
	}

	if (const auto payload = UI::AcceptDragDropPayload(
			NLS::Editor::Assets::kEditorAssetDragPayloadType,
			UI::DragDropTargetFlags::None);
		payload.delivered &&
		payload.data != nullptr &&
		payload.dataSize == sizeof(NLS::Editor::Assets::EditorAssetDragPayload))
	{
		const auto* assetPayload = static_cast<const NLS::Editor::Assets::EditorAssetDragPayload*>(payload.data);
		if (NLS::Editor::Assets::CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(*assetPayload))
		{
			(void)MoveOrCopyProjectBrowserFileIntoFolder(
				NLS::Editor::Assets::GetEditorAssetDragPayloadPath(*assetPayload),
				absoluteFolder);
		}
	}

	if (const auto payload = UI::AcceptDragDropPayload(
			"GameObject",
			UI::DragDropTargetFlags::None);
		payload.delivered &&
		payload.data != nullptr &&
		payload.dataSize == sizeof(std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>))
	{
		const auto* objectPayload = static_cast<const std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>*>(payload.data);
		(void)SaveHierarchyObjectAsPrefabIntoFolder(
			objectPayload->first,
			projectRelativeFolder,
			absoluteFolder);
	}

	UI::EndDragDropTarget();
}

void Editor::Panels::AssetBrowser::OpenProjectGridItemProperties(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	if (!NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(item).canOpenProperties)
		return;

	auto& assetProperties = EDITOR_PANEL(Editor::Panels::AssetProperties, "Asset Properties");
	assetProperties.SetTarget(ProjectBrowserSelectionPathForItem(item));
	assetProperties.Open();
	assetProperties.Focus();
}

void Editor::Panels::AssetBrowser::PreviewProjectGridItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	OpenProjectGridItemProperties(item);
	auto& assetProperties = EDITOR_PANEL(Editor::Panels::AssetProperties, "Asset Properties");
	auto& assetView = EDITOR_PANEL(Editor::Panels::AssetView, "Asset View");
	assetProperties.Preview();
	assetView.Open();
	assetView.Focus();
}

void Editor::Panels::AssetBrowser::RebuildProjectAssetPresentationAfterWorkflow()
{
	m_refreshRequested = false;
	RefreshPreservingExpandedFolders();
}

void Editor::Panels::AssetBrowser::ScheduleProjectAssetPreimportForPath(
	std::string projectRelativePath)
{
	projectRelativePath = NormalizeProjectBrowserPath(std::move(projectRelativePath));
	if (projectRelativePath.empty())
		return;

	ScheduleProjectAssetPreimport({
		NLS::Editor::Assets::AssetPreimportReason::AssetCopiedOrMoved,
		{ std::move(projectRelativePath) }
	});
}

void Editor::Panels::AssetBrowser::ScheduleProjectAssetPreimportForPath(
	const std::filesystem::path& projectRelativePath)
{
	if (projectRelativePath.empty())
		return;

	ScheduleProjectAssetPreimportForPath(NormalizeProjectBrowserPath(projectRelativePath));
}

bool Editor::Panels::AssetBrowser::MoveOrCopyProjectBrowserFolderIntoFolder(
	const std::string& receivedProjectRelativeFolder,
	const std::filesystem::path& targetAbsoluteFolder)
{
	if (receivedProjectRelativeFolder.empty() || targetAbsoluteFolder.empty())
		return false;

	const auto target = targetAbsoluteFolder.lexically_normal();
	if (!NLS::Editor::Assets::CanMoveProjectBrowserResourcePathIntoFolder(
			m_projectAssetFolder,
			receivedProjectRelativeFolder,
			target,
			true))
	{
		return false;
	}

	const auto source = ProjectBrowserAbsolutePathForResourcePath(m_projectAssetFolder, receivedProjectRelativeFolder);
	if (source.empty() || !std::filesystem::is_directory(source))
		return false;

	const auto destination = (target / source.filename()).lexically_normal();
	if (source == destination)
		return false;

	if (IsPathInsideOrEqual(target, source))
	{
		using namespace NLS::Dialogs;
		MessageBox errorMessage(
			"Invalid folder move",
			"You can't move a folder into itself.",
			MessageBox::EMessageType::ERROR,
			MessageBox::EButtonLayout::OK);
		return false;
	}

	if (std::filesystem::exists(destination))
	{
		using namespace NLS::Dialogs;
		MessageBox errorMessage(
			"Folder already exists",
			"You can't move this folder to this location because the name is already taken.",
			MessageBox::EMessageType::ERROR,
			MessageBox::EButtonLayout::OK);
		return false;
	}

	RenameAsset(source.string(), EnsureTrailingPathSeparator(destination));
	EDITOR_EXEC(PropagateFolderRename(source.string(), EnsureTrailingPathSeparator(destination)));
	ScheduleProjectAssetPreimportForPath(EditorAssetFolderFromAbsolutePath(m_projectAssetFolder, destination.string()));
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

bool Editor::Panels::AssetBrowser::MoveOrCopyProjectBrowserFileIntoFolder(
	const std::string& receivedResourcePath,
	const std::filesystem::path& targetAbsoluteFolder)
{
	if (receivedResourcePath.empty() || targetAbsoluteFolder.empty())
		return false;

	const auto target = targetAbsoluteFolder.lexically_normal();
	if (!NLS::Editor::Assets::CanMoveProjectBrowserResourcePathIntoFolder(
			m_projectAssetFolder,
			receivedResourcePath,
			target,
			false))
	{
		return false;
	}

	const auto source = ProjectBrowserAbsolutePathForResourcePath(m_projectAssetFolder, receivedResourcePath);
	if (source.empty() || !std::filesystem::is_regular_file(source))
		return false;

	const auto destination = (target / source.filename()).lexically_normal();
	if (source == destination)
		return false;
	if (std::filesystem::exists(destination))
	{
		using namespace NLS::Dialogs;
		MessageBox errorMessage(
			"File already exists",
			"You can't move this file to this location because the name is already taken.",
			MessageBox::EMessageType::ERROR,
			MessageBox::EButtonLayout::OK);
		return false;
	}

	RenameAsset(source.string(), destination.string());
	EDITOR_EXEC(PropagateFileRename(source.string(), destination.string()));

	ScheduleProjectAssetPreimportForPath(EditorAssetPathFromAbsolutePath(m_projectAssetFolder, destination.string()));
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

bool Editor::Panels::AssetBrowser::SaveHierarchyObjectAsPrefabIntoFolder(
	Engine::GameObject* gameObject,
	const std::string& targetProjectRelativeFolder,
	const std::filesystem::path& targetAbsoluteFolder)
{
	if (gameObject == nullptr || targetProjectRelativeFolder.empty() || targetAbsoluteFolder.empty())
		return false;

	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	if (projectRoot.empty())
	{
		NLS_LOG_ERROR("Failed to resolve prefab destination project root for hierarchy drop.");
		return false;
	}

	NLS::Editor::Assets::AssetDatabaseFacade database(
		NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
	if (!database.Refresh())
	{
		NLS_LOG_ERROR("Failed to refresh asset database before saving prefab from hierarchy drop.");
		return false;
	}

	NLS::Core::Assets::AssetId sceneAssetId;
	const auto currentSceneSourcePath = EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath();
	if (!currentSceneSourcePath.empty())
	{
		const auto sceneMeta = NLS::Core::Assets::AssetMeta::Load(
			NLS::Core::Assets::GetAssetMetaPath(std::filesystem::path(currentSceneSourcePath).lexically_normal()));
		if (sceneMeta.has_value())
			sceneAssetId = sceneMeta->id;
	}

	const auto result = NLS::Editor::Assets::AssetDragDropWorkflow().Execute({
		{NLS::Editor::Assets::DragPayloadKind::HierarchyObject, {}, {}, nullptr, gameObject},
		{NLS::Editor::Assets::DropTargetKind::AssetBrowserFolder, nullptr, nullptr, 0u, false, targetProjectRelativeFolder},
		sceneAssetId,
		NLS::Editor::Assets::DragDropOperationKind::SaveAsPrefab,
		&database,
		&EDITOR_CONTEXT(prefabInstanceRegistry)
	});

	if (result.status != NLS::Editor::Assets::DragDropOperationStatus::Committed)
	{
		for (const auto& diagnostic : result.diagnostics)
			NLS_LOG_ERROR(diagnostic.code + ": " + diagnostic.message);
		return false;
	}

	EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
	if (result.instance.has_value() && result.instance->instanceRoot != nullptr)
	{
		EDITOR_PANEL(NLS::Editor::Panels::Hierarchy, "Hierarchy")
			.RefreshPrefabPresentation(*result.instance->instanceRoot);
	}
	for (const auto& createdPath : result.createdAssetPaths)
		ScheduleProjectAssetPreimportForPath(createdPath);
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

void Editor::Panels::AssetBrowser::SelectProjectGridItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	m_selectedProjectItem = item.projectRelativePath;
	if (!NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(item).canOpenProperties)
	{
		return;
	}

	OpenProjectGridItemProperties(item);
}

void Editor::Panels::AssetBrowser::OpenProjectGridItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	using NLS::Editor::Assets::AssetBrowserItemKind;
	using NLS::Editor::Assets::AssetBrowserItemType;

	if (item.kind == AssetBrowserItemKind::Folder)
	{
		SelectProjectFolder(item.projectRelativePath);
		return;
	}

	if (item.dragResourcePath.empty())
		return;

	if (item.type == AssetBrowserItemType::Scene)
	{
		EDITOR_EXEC(LoadSceneFromDisk(item.absolutePath.string(), true));
		return;
	}

	if (item.type == AssetBrowserItemType::Prefab)
	{
		const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
		NLS::Editor::Assets::AssetDatabaseFacade database(
			NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
		if (!database.Refresh())
		{
			NLS_LOG_ERROR("Failed to refresh asset database before opening prefab: " + item.dragResourcePath);
			return;
		}
		if (!database.IsArtifactManifestCurrentForAssetPath(item.dragResourcePath))
		{
			NLS_LOG_ERROR("Skipped opening stale prefab artifact: " + item.dragResourcePath);
			return;
		}

		const auto prefabSubAssetKey = item.subAssetKey.empty()
			? "prefab:" + std::filesystem::path(item.dragResourcePath).stem().generic_string()
			: item.subAssetKey;
		auto prefab = database.LoadPrefabArtifactAtPath(item.dragResourcePath, prefabSubAssetKey);
		if (!prefab.has_value())
		{
			NLS_LOG_ERROR("Failed to load prefab artifact for prefab stage: " + item.dragResourcePath);
			return;
		}

		auto stage = NLS::Editor::Assets::PrefabUtilityFacade().LoadPrefabContents({
			&*prefab,
			prefab->assetId,
			prefabSubAssetKey,
			prefab->generatedModelPrefab || item.generatedReadOnly,
			item.dragResourcePath
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
		NLS_LOG_INFO("Opened prefab stage: " + item.dragResourcePath);
		return;
	}

	if (item.kind == AssetBrowserItemKind::SourceAsset &&
		item.type == AssetBrowserItemType::Material)
	{
		SelectProjectGridItem(item);
		const auto resourcePath = ProjectBrowserResourcePathForItem(item);
		NLS::Render::Resources::Material* material =
			NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager)[resourcePath];
		if (material != nullptr)
		{
			auto& materialEditor = EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor");
			materialEditor.SetTarget(*material);
			materialEditor.Open();
			materialEditor.Focus();

			auto& assetView = EDITOR_PANEL(Editor::Panels::AssetView, "Asset View");
			assetView.SetResource(material);
			assetView.Open();
			assetView.Focus();
		}
		return;
	}

	SelectProjectGridItem(item);

	if (item.previewableInAssetView)
	{
		auto& assetProperties = EDITOR_PANEL(Editor::Panels::AssetProperties, "Asset Properties");
		auto& assetView = EDITOR_PANEL(Editor::Panels::AssetView, "Asset View");
		assetProperties.Preview();
		assetView.Open();
		assetView.Focus();
	}
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
					if (p_data.first.empty())
						return;

					const std::string correctPath = m_pathUpdate.find(&treeNode) != m_pathUpdate.end() ? m_pathUpdate.at(&treeNode) : path;
					const bool movedOrCopied = MoveOrCopyProjectBrowserFolderIntoFolder(
						p_data.first,
						correctPath);
					if (!movedOrCopied)
						return;

					treeNode.Open();
					treeNode.RemoveAllWidgets();
					ParseFolder(treeNode, std::filesystem::directory_entry(correctPath), p_isEngineItem);

					const bool isEngineFolder = p_data.first.front() == ':';
					if (!isEngineFolder && p_data.second)
						p_data.second->Destroy();
				};

				auto moveOrCopyFileIntoFolder = [this, &treeNode, path, p_isEngineItem](
					const std::string& receivedResourcePath,
					UI::Widgets::Group* receivedGroup)
				{
					if (receivedResourcePath.empty())
						return;

					const std::string correctPath = m_pathUpdate.find(&treeNode) != m_pathUpdate.end() ? m_pathUpdate.at(&treeNode) : path;
					const bool movedOrCopied = MoveOrCopyProjectBrowserFileIntoFolder(
						receivedResourcePath,
						correctPath);
					if (!movedOrCopied)
						return;

					treeNode.Open();
					treeNode.RemoveAllWidgets();
					ParseFolder(treeNode, std::filesystem::directory_entry(correctPath), p_isEngineItem);

					const bool isEngineFile = receivedResourcePath.front() == ':';
					if (!isEngineFile && receivedGroup)
						receivedGroup->Destroy();
				};

				treeNode.AddPlugin<UI::DDTarget<std::pair<std::string, Group*>>>("File").DataReceivedEvent += [moveOrCopyFileIntoFolder](std::pair<std::string, Group*> p_data)
				{
					moveOrCopyFileIntoFolder(p_data.first, p_data.second);
				};

				treeNode.AddPlugin<UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>(
					NLS::Editor::Assets::kEditorAssetDragPayloadType).DataReceivedEvent += [moveOrCopyFileIntoFolder](NLS::Editor::Assets::EditorAssetDragPayload p_data)
				{
					if (NLS::Editor::Assets::CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(p_data))
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

					NLS::Core::Assets::AssetId sceneAssetId;
					const auto currentSceneSourcePath = EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath();
					if (!currentSceneSourcePath.empty())
					{
						const auto sceneMeta = NLS::Core::Assets::AssetMeta::Load(
							NLS::Core::Assets::GetAssetMetaPath(std::filesystem::path(currentSceneSourcePath).lexically_normal()));
						if (sceneMeta.has_value())
							sceneAssetId = sceneMeta->id;
					}

					const auto result = NLS::Editor::Assets::AssetDragDropWorkflow().Execute({
						{NLS::Editor::Assets::DragPayloadKind::HierarchyObject, {}, {}, nullptr, p_data.first},
						{NLS::Editor::Assets::DropTargetKind::AssetBrowserFolder, nullptr, nullptr, 0u, false, destinationFolder},
						sceneAssetId,
						NLS::Editor::Assets::DragDropOperationKind::SaveAsPrefab,
						&database,
						&EDITOR_CONTEXT(prefabInstanceRegistry)
					});

					if (result.status != NLS::Editor::Assets::DragDropOperationStatus::Committed)
					{
						for (const auto& diagnostic : result.diagnostics)
							NLS_LOG_ERROR(diagnostic.code + ": " + diagnostic.message);
						return;
					}

					EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
					if (result.instance.has_value() && result.instance->instanceRoot != nullptr)
					{
						EDITOR_PANEL(NLS::Editor::Panels::Hierarchy, "Hierarchy")
							.RefreshPrefabPresentation(*result.instance->instanceRoot);
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
		const bool assetPayloadReplacesFileDrag =
			assetPayload.has_value() &&
			(fileType == Utils::PathParser::EFileType::MODEL ||
			 fileType == Utils::PathParser::EFileType::PREFAB);
		UI::DDSource<std::pair<std::string, Group*>>* fileDragSource = nullptr;
		if (!assetPayloadReplacesFileDrag)
		{
			fileDragSource = &clickableText.AddPlugin<UI::DDSource<std::pair<std::string, Group*>>>
			(
				"File",
				resourceFormatPath,
				std::make_pair(resourceFormatPath, &itemGroup)
			);
		}
		UI::DDSource<NLS::Editor::Assets::EditorAssetDragPayload>* editorAssetDragSource = nullptr;
		if (assetPayload)
		{
			editorAssetDragSource = &clickableText.AddPlugin<UI::DDSource<NLS::Editor::Assets::EditorAssetDragPayload>>(
				NLS::Editor::Assets::kEditorAssetDragPayloadType,
				resourceFormatPath,
				*assetPayload);
			editorAssetDragSource->hasTooltip = !assetPayloadReplacesFileDrag;
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
					auto& subAssetDragSource = subAssetText.AddPlugin<UI::DDSource<NLS::Editor::Assets::EditorAssetDragPayload>>(
						NLS::Editor::Assets::kEditorAssetDragPayloadType,
						subAsset.dragResourcePath,
						NLS::Editor::Assets::MakeEditorAssetDragPayload(
							subAsset.dragResourcePath,
							subAsset.assetId,
							subAsset.subAssetKey,
							subAsset.artifactType,
							false,
							true,
							false,
							true));
					subAssetDragSource.hasTooltip = false;
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

		contextMenu->RenamedEvent += [
			this,
			fileDragSource,
			editorAssetDragSource,
			&clickableText,
			p_scriptFolder](std::string p_prev, std::string p_newPath)
		{
			if (p_newPath != p_prev)
			{
				if (!std::filesystem::exists(p_newPath))
				{
					RenameAsset(p_prev, p_newPath);
					std::string elementName = Utils::PathParser::GetElementName(p_newPath);
					const auto newResourceFormatPath =
						EditorAssetPathFromAbsolutePath(m_projectAssetFolder, p_newPath).generic_string();
					if (fileDragSource != nullptr)
					{
						fileDragSource->data.first = newResourceFormatPath.empty()
							? Utils::PathParser::GetContainingFolder(fileDragSource->data.first) + elementName
							: newResourceFormatPath;
						fileDragSource->tooltip = fileDragSource->data.first;
					}
					if (editorAssetDragSource != nullptr && !newResourceFormatPath.empty())
					{
						const auto updatedPayload = BuildEditorAssetDragPayloadForFile(
							m_projectAssetFolder,
							p_newPath,
							newResourceFormatPath,
							Utils::PathParser::GetFileType(p_newPath));
						if (updatedPayload.has_value())
						{
							editorAssetDragSource->data = *updatedPayload;
							editorAssetDragSource->tooltip = newResourceFormatPath;
						}
					}

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
				if (!database.IsArtifactManifestCurrentForAssetPath(resourceFormatPath))
				{
					NLS_LOG_ERROR("Skipped opening stale prefab artifact: " + resourceFormatPath);
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
