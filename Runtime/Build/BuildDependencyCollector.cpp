#include "BuildDependencyCollector.h"

#include "BuildFileUtils.h"
#include "BuildReport.h"

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/AssetMeta.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Serialize/ObjectGraphReader.h"
#include "Guid.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <queue>
#include <set>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace NLS::Build
{
namespace
{
using NLS::Core::Assets::ArtifactDatabase;
using NLS::Core::Assets::ArtifactDatabaseRecord;
using NLS::Core::Assets::ArtifactManifest;
using NLS::Core::Assets::AssetDependencyKind;
using NLS::Core::Assets::AssetId;
using NLS::Core::Assets::AssetMeta;
using NLS::Core::Assets::GetAssetMetaPath;
using NLS::Engine::Assets::RuntimeAssetRef;
using NLS::Engine::Assets::RuntimeManifestBuilder;

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string NormalizeRelative(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    while (!value.empty() && value.front() == '/')
        value.erase(value.begin());
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}

std::string ReferenceText(const RuntimeAssetRef& reference)
{
    return reference.assetId.ToString() + ":" + reference.subAssetKey;
}

bool IsBuildDependencyKind(const AssetDependencyKind kind)
{
    return kind == AssetDependencyKind::ImportedArtifact ||
        kind == AssetDependencyKind::PrefabBase ||
        kind == AssetDependencyKind::NestedPrefab;
}

std::optional<RuntimeAssetRef> ToRuntimeRef(const NLS::Core::Assets::AssetDependencyRecord& dependency)
{
    if (!IsBuildDependencyKind(dependency.kind) || dependency.hashOrVersion.empty())
        return std::nullopt;
    const auto guid = NLS::Guid::TryParse(dependency.value);
    if (!guid.has_value())
        return std::nullopt;
    return RuntimeAssetRef {AssetId(*guid), dependency.hashOrVersion};
}

std::vector<std::filesystem::path> EnumerateAssetFiles(
    const std::filesystem::path& root,
    const bool recursive)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error))
        return files;
    if (recursive)
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error))
        {
            if (error)
                break;
            if (entry.is_regular_file())
                files.push_back(entry.path());
        }
    }
    else
    {
        for (const auto& entry : std::filesystem::directory_iterator(root, error))
        {
            if (error)
                break;
            if (entry.is_regular_file())
                files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool HasWildcard(const std::string& pattern)
{
    return pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos;
}
} // namespace

bool BuildDependencyCollector::MatchesWildcard(const std::string& pattern, const std::string& value)
{
    // Iterative glob matching with '*' and '?'.
    size_t patternIndex = 0u;
    size_t valueIndex = 0u;
    size_t starIndex = std::string::npos;
    size_t starValueIndex = 0u;
    while (valueIndex < value.size())
    {
        if (patternIndex < pattern.size() &&
            (pattern[patternIndex] == '?' || pattern[patternIndex] == value[valueIndex]))
        {
            ++patternIndex;
            ++valueIndex;
            continue;
        }
        if (patternIndex < pattern.size() && pattern[patternIndex] == '*')
        {
            starIndex = patternIndex;
            starValueIndex = valueIndex;
            ++patternIndex;
            continue;
        }
        if (starIndex != std::string::npos)
        {
            patternIndex = starIndex + 1u;
            ++starValueIndex;
            valueIndex = starValueIndex;
            continue;
        }
        return false;
    }
    while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
        ++patternIndex;
    return patternIndex == pattern.size();
}

std::filesystem::path BuildDependencyCollector::ResolveArtifactHostPath(
    const std::filesystem::path& projectRoot,
    const std::string& artifactPath)
{
    if (artifactPath.empty())
        return {};
    auto normalized = std::filesystem::path(artifactPath).lexically_normal();
    if (normalized.is_absolute() || normalized.has_root_name())
        return normalized;
    const auto generic = normalized.generic_string();
    if (generic.rfind("Library/", 0u) == 0u)
        return projectRoot / normalized;
    if (generic.rfind("Artifacts/", 0u) == 0u)
        return projectRoot / "Library" / normalized;
    return projectRoot / normalized;
}

std::optional<AssetId> BuildDependencyCollector::LoadAssetIdForPath(
    const std::filesystem::path& projectAssetsPath,
    const std::string& projectRelativePath,
    std::string* error)
{
    const auto normalized = NormalizeRelative(projectRelativePath);
    if (normalized.empty())
    {
        if (error != nullptr)
            *error = "empty asset path";
        return std::nullopt;
    }

    std::vector<std::filesystem::path> candidates;
    candidates.push_back(projectAssetsPath / std::filesystem::path(normalized));
    if (normalized.rfind("Assets/", 0u) == 0u)
        candidates.push_back(projectAssetsPath / std::filesystem::path(normalized.substr(7u)));

    for (const auto& candidate : candidates)
    {
        const auto metaPath = GetAssetMetaPath(candidate);
        std::error_code metaError;
        if (!std::filesystem::is_regular_file(metaPath, metaError))
            continue;
        const auto meta = AssetMeta::Load(metaPath);
        if (meta.has_value() && meta->id.IsValid())
            return meta->id;
    }
    if (error != nullptr)
        *error = "no .meta with a valid asset id for '" + normalized + "'";
    return std::nullopt;
}

bool BuildDependencyCollector::Collect(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& projectAssetsPath,
    const BuildProfile& profile,
    const std::filesystem::path& artifactDatabasePath,
    DependencyCollectionResult& result,
    BuildReport& report)
{
    result = {};

    ArtifactDatabase database;
    if (!database.Load(artifactDatabasePath))
    {
        report.AddDiagnostic(
            DiagnosticSeverity::Error,
            "build-artifactdb-missing",
            "The project has no readable ArtifactDB.",
            artifactDatabasePath.string());
        return false;
    }

    const auto targetPlatform = profile.target.artifactPlatform.empty()
        ? DefaultArtifactPlatform(profile.target.platform)
        : profile.target.artifactPlatform;

    // 1. Duplicate GUID detection and a GUID → source path/type index used to
    // scan raw scene and prefab documents. Two source assets sharing an id make
    // runtime resolution ambiguous, so the build stops instead of guessing.
    std::unordered_map<std::string, std::filesystem::path> sourceByGuid;
    std::unordered_map<std::string, NLS::Core::Assets::AssetType> typeByGuid;
    {
        std::unordered_map<std::string, std::string> idToSource;
        for (const auto& metaFile : EnumerateAssetFiles(projectAssetsPath, true))
        {
            if (metaFile.extension() != ".meta")
                continue;
            const auto meta = AssetMeta::Load(metaFile);
            if (!meta.has_value() || !meta->id.IsValid())
                continue;
            const auto idText = meta->id.ToString();
            std::error_code relativeError;
            const auto relative = std::filesystem::relative(metaFile, projectAssetsPath, relativeError);
            const auto sourceText = relativeError ? metaFile.filename().string() : relative.generic_string();
            const auto inserted = idToSource.emplace(idText, sourceText);
            if (!inserted.second)
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-duplicate-guid",
                    "Duplicate asset GUID found in '" + inserted.first->second + "' and '" + sourceText + "'.",
                    idText);
                continue;
            }
            auto sourcePath = metaFile;
            sourcePath.replace_extension();
            sourceByGuid.emplace(idText, std::filesystem::absolute(sourcePath));
            auto assetType = meta->assetType;
            if (assetType == NLS::Core::Assets::AssetType::Unknown)
                assetType = NLS::Core::Assets::InferAssetType(sourcePath);
            typeByGuid.emplace(idText, assetType);
        }
        if (report.HasErrors())
            return false;
    }

    // 2. Roots: the start scene document, explicit keep rules, and every asset
    // referenced by the raw scene/prefab graph. Scenes and prefabs are shipped
    // as source documents; their referenced assets must resolve to cooked
    // artifacts for the target platform.
    std::vector<std::pair<RuntimeAssetRef, std::string>> roots;
    std::set<std::string> seenRoots;
    const auto addRoot = [&roots, &seenRoots](const RuntimeAssetRef& reference, const std::string& reason)
    {
        const auto key = ReferenceText(reference);
        if (seenRoots.insert(key).second)
            roots.emplace_back(reference, reason);
    };

    const auto findCookedRecord = [&database, &targetPlatform](
        const AssetId& assetId,
        const int64_t localIdentifierInFile,
        const bool requireCooked) -> const ArtifactDatabaseRecord*
    {
        const ArtifactDatabaseRecord* primary = nullptr;
        for (const auto* record : database.FindBySource(assetId))
        {
            if (record == nullptr || record->targetPlatform != targetPlatform)
                continue;
            if (requireCooked &&
                (record->status != NLS::Core::Assets::ArtifactRecordStatus::UpToDate ||
                    record->artifactPath.empty()))
            {
                continue;
            }
            if (localIdentifierInFile != 0 &&
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(assetId.GetGuid(), record->subAssetKey) ==
                    localIdentifierInFile)
            {
                return record;
            }
            if (record->primarySubAssetKey == record->subAssetKey)
                primary = record;
            else if (primary == nullptr)
                primary = record;
        }
        return primary;
    };

    std::deque<std::filesystem::path> documentsToScan;
    std::set<std::string> queuedDocuments;
    std::map<std::string, std::string> rawDocumentReasons;
    const auto addRawDocument = [&](const std::filesystem::path& path, const std::string& reason)
    {
        const auto normalized = std::filesystem::absolute(path).lexically_normal();
        const auto key = normalized.generic_string();
        rawDocumentReasons.emplace(key, reason);
        if (queuedDocuments.insert(key).second)
            documentsToScan.push_back(normalized);
    };

    const auto addReferencedAsset = [&](const NLS::Engine::Serialize::ObjectIdentifier& identifier,
                                        const std::string& reason) -> bool
    {
        if (!identifier.guid.IsValid())
            return true;
        const auto guidText = identifier.guid.ToString();
        const auto typeIt = typeByGuid.find(guidText);
        const auto isDocument = typeIt != typeByGuid.end() &&
            (typeIt->second == NLS::Core::Assets::AssetType::Prefab ||
                typeIt->second == NLS::Core::Assets::AssetType::Scene);
        if (isDocument)
        {
            const auto sourceIt = sourceByGuid.find(guidText);
            if (sourceIt != sourceByGuid.end())
                addRawDocument(sourceIt->second, reason);
            // A cooked prefab artifact is optional at runtime but contributes
            // prewarm dependency edges when the editor produced one.
            if (const auto* record = findCookedRecord(AssetId(identifier.guid), identifier.localIdentifierInFile, false);
                record != nullptr)
            {
                addRoot({AssetId(identifier.guid), record->subAssetKey}, "document-of:" + guidText);
            }
            return true;
        }

        const auto* record = findCookedRecord(AssetId(identifier.guid), identifier.localIdentifierInFile, true);
        if (record == nullptr)
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-reference-not-cooked",
                "A scene or prefab references an asset that has no up-to-date cooked artifact for target '" +
                    targetPlatform + "'. Run 'NullusBuild bake' or import the asset first.",
                guidText);
            return false;
        }
        addRoot({AssetId(identifier.guid), record->subAssetKey}, reason);
        return true;
    };

    const auto startSceneRelative = NormalizeRelative(profile.startScene);
    if (startSceneRelative.empty())
    {
        report.AddDiagnostic(
            DiagnosticSeverity::Error,
            "build-start-scene-missing",
            "The build profile has no start scene.",
            profile.id);
        return false;
    }

    const auto startScenePath = projectAssetsPath / std::filesystem::path(startSceneRelative);
    std::error_code sceneError;
    if (!std::filesystem::is_regular_file(startScenePath, sceneError))
    {
        report.AddDiagnostic(
            DiagnosticSeverity::Error,
            "build-start-scene-missing",
            "The start scene file does not exist.",
            startScenePath.string());
        return false;
    }
    addRawDocument(startScenePath, "start-scene");

    // Keep rules add documents or cooked roots. A rule may name a GUID, a file,
    // a directory, or a wildcard pattern.
    const auto addRuleAsset = [&](const std::filesystem::path& sourcePath, const std::string& ruleReason) -> bool
    {
        std::error_code metaError;
        const auto metaPath = GetAssetMetaPath(sourcePath);
        const auto meta = AssetMeta::Load(metaPath);
        auto assetType = meta.has_value() ? meta->assetType : NLS::Core::Assets::AssetType::Unknown;
        if (assetType == NLS::Core::Assets::AssetType::Unknown)
            assetType = NLS::Core::Assets::InferAssetType(sourcePath);

        if (assetType == NLS::Core::Assets::AssetType::Prefab ||
            assetType == NLS::Core::Assets::AssetType::Scene)
        {
            addRawDocument(sourcePath, ruleReason);
            if (meta.has_value() && meta->id.IsValid())
            {
                if (const auto* record = findCookedRecord(meta->id, 0, false); record != nullptr)
                    addRoot({meta->id, record->subAssetKey}, ruleReason);
            }
            return true;
        }

        if (!meta.has_value() || !meta->id.IsValid())
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-keep-rule-no-meta",
                "Keep rule matched an asset without a valid .meta GUID.",
                sourcePath.string());
            return false;
        }
        const auto* record = findCookedRecord(meta->id, 0, true);
        if (record == nullptr)
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-keep-rule-not-cooked",
                "Keep rule matched an asset with no up-to-date cooked artifact for target '" +
                    targetPlatform + "'.",
                sourcePath.string());
            return false;
        }
        addRoot({meta->id, record->subAssetKey}, ruleReason);
        return true;
    };
    (void)addRuleAsset;

    for (const auto& rule : profile.keepAssetPaths)
    {
        const auto trimmed = NormalizeRelative(rule);
        if (trimmed.empty())
            continue;
        const auto ruleReason = "keep-rule:" + trimmed;
        if (const auto guid = NLS::Guid::TryParse(trimmed); guid.has_value())
        {
            const auto sourceIt = sourceByGuid.find(guid->ToString());
            if (sourceIt != sourceByGuid.end())
            {
                if (addRuleAsset(sourceIt->second, ruleReason))
                    continue;
            }
            const auto* record = findCookedRecord(AssetId(*guid), 0, true);
            if (record != nullptr)
            {
                addRoot({AssetId(*guid), record->subAssetKey}, ruleReason);
                continue;
            }
            report.AddDiagnostic(
                DiagnosticSeverity::Warning,
                "build-keep-rule-unresolved",
                "GUID keep rule could not be resolved to a cooked asset.",
                trimmed);
            continue;
        }

        const auto fullPath = projectAssetsPath / std::filesystem::path(trimmed);
        std::error_code error;
        if (std::filesystem::is_directory(fullPath, error))
        {
            for (const auto& file : EnumerateAssetFiles(fullPath, true))
            {
                if (file.extension() == ".meta")
                    continue;
                addRuleAsset(file, ruleReason);
            }
            continue;
        }
        if (std::filesystem::is_regular_file(fullPath, error) ||
            HasWildcard(trimmed))
        {
            if (!HasWildcard(trimmed))
            {
                addRuleAsset(fullPath, ruleReason);
            }
            else
            {
                for (const auto& file : EnumerateAssetFiles(projectAssetsPath, true))
                {
                    if (file.extension() == ".meta")
                        continue;
                    std::error_code relativeError;
                    const auto relative = std::filesystem::relative(file, projectAssetsPath, relativeError);
                    if (relativeError)
                        continue;
                    if (MatchesWildcard(trimmed, relative.generic_string()))
                        addRuleAsset(file, ruleReason);
                }
            }
            continue;
        }
        report.AddDiagnostic(
            DiagnosticSeverity::Warning,
            "build-keep-rule-unresolved",
            "Keep rule could not be resolved to a project asset.",
            trimmed);
    }

    // 3. Scan raw scene/prefab documents and resolve their asset references.
    while (!documentsToScan.empty())
    {
        const auto documentPath = documentsToScan.front();
        documentsToScan.pop_front();
        const auto documentKey = documentPath.generic_string();

        const auto text = ReadTextFileOrEmpty(documentPath);
        auto document = text.empty()
            ? std::nullopt
            : NLS::Engine::Serialize::ObjectGraphReader::Read(text);
        if (!document.has_value())
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                documentKey == std::filesystem::absolute(startScenePath).lexically_normal().generic_string()
                    ? "build-start-scene-invalid"
                    : "build-document-invalid",
                "The serialized scene/prefab document could not be parsed.",
                documentPath.string());
            continue;
        }

        std::vector<NLS::Engine::Serialize::ObjectIdentifier> references =
            NLS::Engine::Assets::CollectPrefabAssetReferences(*document);
        for (const auto& instance : document->prefabInstances)
            references.push_back(instance.sourcePrefab);

        std::sort(
            references.begin(),
            references.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.guid.ToString() < rhs.guid.ToString(); });
        references.erase(
            std::unique(
                references.begin(),
                references.end(),
                [](const auto& lhs, const auto& rhs)
                {
                    return lhs.guid == rhs.guid &&
                        lhs.localIdentifierInFile == rhs.localIdentifierInFile;
                }),
            references.end());

        const auto reason = rawDocumentReasons[documentKey];
        for (const auto& reference : references)
            (void)addReferencedAsset(reference, reason + ":reference");
    }

    // 3. Dependency closure for every root at the target platform.
    RuntimeManifestBuilder builder;
    std::map<std::string, std::vector<RuntimeAssetRef>> dependenciesBySource;
    for (const auto& record : database.GetRecords())
    {
        if (record.targetPlatform != targetPlatform ||
            record.status != NLS::Core::Assets::ArtifactRecordStatus::UpToDate)
        {
            continue;
        }
        const auto key = record.sourceAssetId.ToString();
        if (dependenciesBySource.find(key) != dependenciesBySource.end())
            continue;
        auto manifest = database.BuildManifestForSource(record.sourceAssetId, targetPlatform);
        if (!manifest.has_value())
            continue;
        std::vector<RuntimeAssetRef> dependencies;
        for (const auto& dependency : manifest->dependencies)
        {
            const auto reference = ToRuntimeRef(dependency);
            if (reference.has_value())
                dependencies.push_back(*reference);
        }
        dependenciesBySource[key] = std::move(dependencies);
        builder.AddArtifactManifest(std::move(*manifest));
    }

    std::vector<RuntimeAssetRef> rootRefs;
    std::unordered_map<std::string, std::string> reasonByReference;
    for (const auto& [reference, reason] : roots)
    {
        if (reasonByReference.find(ReferenceText(reference)) == reasonByReference.end())
        {
            reasonByReference[ReferenceText(reference)] = reason;
            rootRefs.push_back(reference);
        }
    }

    auto buildResult = builder.Build(rootRefs, targetPlatform);

    // The builder normalises GUID-only roots to their manifest primary
    // sub-asset. Re-key the recorded reasons so attribution starts from the
    // same references the manifest actually contains.
    {
        std::vector<std::string> rootReasons;
        rootReasons.reserve(rootRefs.size());
        for (const auto& reference : rootRefs)
            rootReasons.push_back(reasonByReference[ReferenceText(reference)]);
        reasonByReference.clear();
        for (size_t index = 0u;
             index < buildResult.manifest.roots.size() && index < rootReasons.size();
             ++index)
        {
            reasonByReference[ReferenceText(buildResult.manifest.roots[index])] = rootReasons[index];
        }
    }

    for (const auto& diagnostic : buildResult.diagnostics.GetItems())
    {
        report.AddDiagnostic(
            diagnostic.severity == NLS::Core::Assets::ImportDiagnosticSeverity::Error
                ? DiagnosticSeverity::Error
                : DiagnosticSeverity::Warning,
            diagnostic.code,
            "Runtime manifest validation failed for asset '" + diagnostic.assetId.ToString() + "'.",
            diagnostic.subAssetKey + " " + diagnostic.path);
    }
    if (report.HasErrors())
        return false;

    // 4. Attribute every reachable reference to the root that introduced it so
    // the build report explains why each asset is included.
    {
        std::queue<std::pair<RuntimeAssetRef, std::string>> pending;
        for (const auto& reference : rootRefs)
            pending.push({reference, reasonByReference[ReferenceText(reference)]});

        while (!pending.empty())
        {
            const auto [reference, reason] = pending.front();
            pending.pop();
            const auto key = reference.assetId.ToString();
            const auto found = dependenciesBySource.find(key);
            if (found == dependenciesBySource.end())
                continue;
            for (const auto& dependency : found->second)
            {
                const auto dependencyKey = ReferenceText(dependency);
                if (reasonByReference.find(dependencyKey) != reasonByReference.end())
                    continue;
                std::string childReason = "dependency-of:" + reference.subAssetKey;
                reasonByReference[dependencyKey] = childReason;
                pending.push({dependency, std::move(childReason)});
            }
        }
    }

    result.manifest = std::move(buildResult.manifest);
    result.manifest.targetPlatform = targetPlatform;

    // 5. Resolve artifact files on disk and verify they exist.
    std::unordered_set<std::string> includedArtifacts;
    for (const auto& entry : result.manifest.entries)
    {
        const auto hostPath = ResolveArtifactHostPath(projectRoot, entry.artifactPath);
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(hostPath, sizeError);
        if (sizeError)
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-artifact-file-missing",
                "Cooked artifact file is missing for asset '" + entry.assetId.ToString() + "'.",
                hostPath.string());
            continue;
        }
        CollectedArtifact artifact;
        artifact.reference = ReferenceText({entry.assetId, entry.subAssetKey});
        artifact.artifactPath = entry.artifactPath;
        artifact.hostPath = hostPath;
        artifact.size = size;
        artifact.package = "base";
        const auto reason = reasonByReference.find(artifact.reference);
        artifact.reason = reason != reasonByReference.end() ? reason->second : "dependency";
        includedArtifacts.insert(entry.artifactPath);
        result.artifacts.push_back(std::move(artifact));
    }
    if (report.HasErrors())
        return false;

    // 6. Raw assets that phase-1 runtime readers still load as loose files.
    {
        std::set<std::string> rawExtensions;
        for (const auto& extension : profile.rawAssetExtensions)
            rawExtensions.insert(ToLower(extension));
        std::set<std::string> rawFilesSeen;

        const auto addRawFile = [&result, &rawFilesSeen](const std::filesystem::path& path, const std::string& reason)
        {
            const auto normalized = std::filesystem::absolute(path).lexically_normal();
            if (!rawFilesSeen.insert(normalized.generic_string()).second)
                return;
            std::error_code sizeError;
            const auto size = std::filesystem::file_size(path, sizeError);
            if (sizeError)
                return;
            CollectedRawFile file;
            file.hostPath = path;
            file.size = size;
            file.reason = reason;
            result.rawFiles.push_back(std::move(file));
        };

        for (const auto& file : EnumerateAssetFiles(projectAssetsPath, true))
        {
            const auto extension = ToLower(file.extension().string());
            if (rawExtensions.find(extension) != rawExtensions.end())
                addRawFile(file, "raw-asset");
        }
        for (const auto& [rawPath, reason] : rawDocumentReasons)
            addRawFile(std::filesystem::path(rawPath), reason);
    }

    // 7. Explicit pack selections become separate packages. Shared
    // dependencies stay in the base package.
    if (!profile.assetPackSelections.empty())
    {
        std::vector<NLS::Engine::Assets::AssetPackBuildInput> packInputs;
        for (const auto& selection : profile.assetPackSelections)
        {
            const auto separator = selection.find(':');
            const auto packName = separator == std::string::npos
                ? selection
                : selection.substr(0u, separator);
            const auto packVariant = separator == std::string::npos
                ? std::string {}
                : selection.substr(separator + 1u);
            if (packName.empty())
                continue;

            const auto packDirectory = projectAssetsPath / std::filesystem::path(packName);
            std::error_code packError;
            if (!std::filesystem::is_directory(packDirectory, packError))
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-asset-pack-missing",
                    "Asset pack selection '" + selection + "' has no matching Assets directory.",
                    packName);
                continue;
            }
            for (const auto& metaFile : EnumerateAssetFiles(packDirectory, true))
            {
                if (metaFile.extension() != ".meta")
                    continue;
                const auto meta = AssetMeta::Load(metaFile);
                if (meta.has_value() && meta->id.IsValid())
                    packInputs.push_back({packName, packVariant, {meta->id, std::string {}}});
            }
        }
        if (report.HasErrors())
            return false;

        if (!packInputs.empty())
        {
            auto packResult = builder.BuildAssetPacks(packInputs, targetPlatform);
            result.manifest.assetPacks = packResult.manifest.assetPacks;

            for (const auto& pack : result.manifest.assetPacks)
            {
                std::vector<std::string> packageArtifacts;
                for (const auto& entry : pack.entries)
                {
                    if (includedArtifacts.find(entry.artifactPath) != includedArtifacts.end())
                        continue;
                    includedArtifacts.insert(entry.artifactPath);
                    packageArtifacts.push_back(entry.artifactPath);
                }
                result.packageArtifactPaths.emplace_back(pack.packName, std::move(packageArtifacts));
            }

            // Merge pack-only entries into the runtime index so scene loads can
            // resolve them through the runtime database.
            for (const auto& pack : result.manifest.assetPacks)
            {
                for (const auto& packEntry : pack.entries)
                {
                    const auto existing = std::find_if(
                        result.manifest.entries.begin(),
                        result.manifest.entries.end(),
                        [&packEntry](const NLS::Engine::Assets::RuntimeAssetManifestEntry& entry)
                        {
                            return entry.assetId == packEntry.reference.assetId &&
                                entry.subAssetKey == packEntry.reference.subAssetKey;
                        });
                    if (existing != result.manifest.entries.end())
                        continue;
                    NLS::Engine::Assets::RuntimeAssetManifestEntry entry;
                    entry.assetId = packEntry.reference.assetId;
                    entry.subAssetKey = packEntry.reference.subAssetKey;
                    entry.artifactType = packEntry.artifactType;
                    entry.loaderId = packEntry.loaderId;
                    entry.artifactPath = packEntry.artifactPath;
                    entry.contentHash = packEntry.contentHash;
                    entry.dependencies = packEntry.dependencies;
                    result.manifest.entries.push_back(std::move(entry));

                    const auto hostPath = ResolveArtifactHostPath(projectRoot, packEntry.artifactPath);
                    std::error_code sizeError;
                    const auto size = std::filesystem::file_size(hostPath, sizeError);
                    if (sizeError)
                    {
                        report.AddDiagnostic(
                            DiagnosticSeverity::Error,
                            "build-artifact-file-missing",
                            "Cooked artifact file is missing for asset pack entry.",
                            hostPath.string());
                        continue;
                    }
                    CollectedArtifact artifact;
                    artifact.reference = ReferenceText(packEntry.reference);
                    artifact.artifactPath = packEntry.artifactPath;
                    artifact.hostPath = hostPath;
                    artifact.size = size;
                    artifact.package = pack.packName;
                    artifact.reason = "asset-pack:" + pack.packName;
                    result.artifacts.push_back(std::move(artifact));
                }
            }
        }
    }

    return !report.HasErrors();
}
} // namespace NLS::Build
