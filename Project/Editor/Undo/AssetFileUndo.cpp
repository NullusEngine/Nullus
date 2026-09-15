#include "Undo/AssetFileUndo.h"
#include "Undo/AssetUndoScope.h"
#include "Undo/EditorUndoMutationContext.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <Assets/AssetMeta.h>

namespace NLS::Editor::Undo::AssetFile
{
namespace
{
struct FileFingerprint
{
    uint64_t size = 0u;
    uint64_t hash = 1469598103934665603ull;
    bool exists = false;

    [[nodiscard]] bool operator==(const FileFingerprint&) const = default;
};

struct AssetFileBundle
{
    std::filesystem::path source;
    std::filesystem::path meta;
    FileFingerprint sourceFingerprint;
    std::optional<FileFingerprint> metaFingerprint;
};

struct TreeFileSnapshot
{
    // For a file root an empty path denotes the source file itself. Directory
    // roots store paths relative to the directory and keep their companion
    // meta file separately.
    std::filesystem::path relativePath;
    std::string contents;
    FileFingerprint fingerprint;
};

struct TreeSnapshot
{
    bool directory = false;
    std::vector<std::filesystem::path> directories;
    std::vector<TreeFileSnapshot> files;
    std::optional<TreeFileSnapshot> rootMeta;
};

struct ReferenceFileSnapshot
{
    std::filesystem::path beforePath;
    std::filesystem::path afterPath;
    std::string beforeContents;
    std::string afterContents;
    FileFingerprint beforeFingerprint;
    FileFingerprint afterFingerprint;
};

[[nodiscard]] std::filesystem::path Normalize(const std::filesystem::path& path)
{
    return path.lexically_normal();
}

[[nodiscard]] std::filesystem::path MetaPathFor(const std::filesystem::path& source)
{
    return std::filesystem::path(source.generic_string() + ".meta");
}

[[nodiscard]] std::optional<FileFingerprint> ReadFingerprint(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return std::nullopt;

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    FileFingerprint result;
    result.exists = true;
    std::array<char, 8192u> buffer {};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index)
        {
            result.hash ^= static_cast<uint8_t>(buffer[static_cast<size_t>(index)]);
            result.hash *= 1099511628211ull;
        }
        result.size += static_cast<uint64_t>(count);
    }
    return input.eof() ? std::optional<FileFingerprint>(result) : std::nullopt;
}

[[nodiscard]] FileFingerprint MakeFingerprint(std::string_view contents)
{
    FileFingerprint result;
    result.exists = true;
    result.size = static_cast<uint64_t>(contents.size());
    for (const unsigned char character : contents)
    {
        result.hash ^= character;
        result.hash *= 1099511628211ull;
    }
    return result;
}

[[nodiscard]] std::optional<std::string> ReadContents(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    std::string contents {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    return input.good() || input.eof() ? std::optional<std::string>(std::move(contents)) : std::nullopt;
}

[[nodiscard]] bool WriteContentsAtomically(
    const std::filesystem::path& path,
    const std::string_view contents)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("writing a serialized asset reference snapshot");
    std::error_code error;
    if (path.parent_path().empty() || !std::filesystem::is_directory(path.parent_path(), error) || error)
        return false;

    const auto temporary = std::filesystem::path(
        path.generic_string() + ".nullus-undo-tmp-" +
        std::to_string(std::hash<std::string> {}(path.generic_string() + std::to_string(contents.size()))));
    std::filesystem::remove(temporary, error);
    error.clear();

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        if (!contents.empty())
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output)
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    // POSIX rename replaces atomically. Windows does not allow that form when
    // the target exists, so use a copy replacement there and rely on the
    // command's exact before/after rollback if the replacement fails.
    std::filesystem::rename(temporary, path, error);
    if (!error)
        return true;

    error.clear();
    std::filesystem::copy_file(
        temporary,
        path,
        std::filesystem::copy_options::overwrite_existing,
        error);
    std::error_code removeError;
    std::filesystem::remove(temporary, removeError);
    return !error && !removeError;
}

[[nodiscard]] bool IsDescendantPath(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate)
{
    const auto relative = Normalize(candidate).lexically_relative(Normalize(root));
    if (relative.empty() || relative == ".")
        return false;
    for (const auto& segment : relative)
    {
        if (segment == "..")
            return false;
    }
    return true;
}

[[nodiscard]] std::filesystem::path SnapshotFilePath(
    const TreeSnapshot& snapshot,
    const std::filesystem::path& root,
    const TreeFileSnapshot& file)
{
    return snapshot.directory ? root / file.relativePath : root;
}

[[nodiscard]] std::optional<TreeSnapshot> CaptureTreeSnapshot(const std::filesystem::path& root)
{
    std::error_code error;
    TreeSnapshot snapshot;
    snapshot.directory = std::filesystem::is_directory(root, error) && !error;
    if (!snapshot.directory && (!std::filesystem::is_regular_file(root, error) || error))
        return std::nullopt;

    const auto appendFile = [&snapshot, &root](const std::filesystem::path& path) -> bool
    {
        const auto contents = ReadContents(path);
        if (!contents.has_value())
            return false;
        TreeFileSnapshot file;
        file.relativePath = snapshot.directory ? path.lexically_relative(root) : std::filesystem::path {};
        if (file.relativePath.empty() && snapshot.directory)
            return false;
        file.contents = *contents;
        file.fingerprint = MakeFingerprint(file.contents);
        snapshot.files.push_back(std::move(file));
        return true;
    };

    if (snapshot.directory)
    {
        for (std::filesystem::recursive_directory_iterator iterator(
                 root,
                 std::filesystem::directory_options::skip_permission_denied,
                 error),
             end;
             iterator != end && !error;
             iterator.increment(error))
        {
            const auto& entry = *iterator;
            if (entry.is_directory(error) && !error)
            {
                snapshot.directories.push_back(entry.path().lexically_relative(root));
                continue;
            }
            error.clear();
            if (!entry.is_regular_file(error) || error || !appendFile(entry.path()))
                return std::nullopt;
        }
        if (error)
            return std::nullopt;
    }
    else if (!appendFile(root))
    {
        return std::nullopt;
    }

    const auto metaPath = MetaPathFor(root);
    if (std::filesystem::exists(metaPath, error))
    {
        if (error || !std::filesystem::is_regular_file(metaPath, error) || error)
            return std::nullopt;
        const auto contents = ReadContents(metaPath);
        if (!contents.has_value())
            return std::nullopt;
        snapshot.rootMeta = TreeFileSnapshot {{}, *contents, MakeFingerprint(*contents)};
    }
    else if (error)
    {
        return std::nullopt;
    }

    std::sort(snapshot.directories.begin(), snapshot.directories.end());
    std::sort(snapshot.files.begin(), snapshot.files.end(), [](const TreeFileSnapshot& left, const TreeFileSnapshot& right)
    {
        return left.relativePath.generic_string() < right.relativePath.generic_string();
    });
    return snapshot;
}

[[nodiscard]] bool TreeMatches(
    const TreeSnapshot& expected,
    const std::filesystem::path& root)
{
    const auto current = CaptureTreeSnapshot(root);
    if (!current.has_value() || current->directory != expected.directory ||
        current->directories != expected.directories || current->files.size() != expected.files.size() ||
        current->rootMeta.has_value() != expected.rootMeta.has_value())
    {
        return false;
    }

    for (size_t index = 0u; index < expected.files.size(); ++index)
    {
        const auto& actual = current->files[index];
        const auto& wanted = expected.files[index];
        if (actual.relativePath != wanted.relativePath || actual.fingerprint != wanted.fingerprint)
            return false;
    }
    return !expected.rootMeta.has_value() ||
        current->rootMeta->fingerprint == expected.rootMeta->fingerprint;
}

[[nodiscard]] bool TreeDestinationIsVacant(
    const std::filesystem::path& destination)
{
    std::error_code error;
    if (std::filesystem::exists(destination, error) || error)
        return false;
    error.clear();
    return !std::filesystem::exists(MetaPathFor(destination), error) && !error;
}

[[nodiscard]] bool IsPathInTree(
    const TreeSnapshot& tree,
    const std::filesystem::path& root,
    const std::filesystem::path& path)
{
    if (!tree.directory)
        return Normalize(path) == Normalize(root);
    return IsDescendantPath(root, path);
}

[[nodiscard]] std::filesystem::path TranslateTreePath(
    const TreeSnapshot& tree,
    const std::filesystem::path& beforeRoot,
    const std::filesystem::path& afterRoot,
    const std::filesystem::path& beforePath)
{
    if (!tree.directory)
        return afterRoot;
    return afterRoot / Normalize(beforePath).lexically_relative(Normalize(beforeRoot));
}

[[nodiscard]] std::vector<std::string> ReferenceAliases(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& path)
{
    const auto normalized = Normalize(path);
    std::vector<std::string> aliases {normalized.generic_string()};
    const auto assetsRoot = Normalize(projectRoot / "Assets");
    if (!IsDescendantPath(assetsRoot, normalized))
        return aliases;

    const auto assetsRelative = normalized.lexically_relative(assetsRoot).generic_string();
    if (assetsRelative.empty())
        return aliases;

    aliases.push_back("/" + assetsRelative);
    aliases.push_back("Assets/" + assetsRelative);
    aliases.push_back(assetsRelative);
    std::sort(aliases.begin(), aliases.end());
    aliases.erase(std::unique(aliases.begin(), aliases.end()), aliases.end());
    return aliases;
}

[[nodiscard]] std::string RewriteReferenceTokens(
    const std::string_view contents,
    const std::map<std::string, std::string, std::less<>>& replacements)
{
    std::string rewritten;
    size_t scanCursor = 0u;
    size_t copiedCursor = 0u;
    bool changed = false;
    while (scanCursor < contents.size())
    {
        const auto open = contents.find('>', scanCursor);
        if (open == std::string_view::npos)
            break;
        const auto close = contents.find('<', open + 1u);
        if (close == std::string_view::npos)
            break;

        const auto found = replacements.find(std::string(contents.substr(open + 1u, close - open - 1u)));
        // Scan independently from the next source span to copy. An unmatched
        // token still belongs in that span when a later token is replaced.
        scanCursor = close + 1u;
        if (found == replacements.end())
            continue;

        changed = true;
        rewritten.append(contents.substr(copiedCursor, open + 1u - copiedCursor));
        rewritten.append(found->second);
        // Retain the closing delimiter in the next copied span.
        copiedCursor = close;
    }

    if (!changed)
        return std::string(contents);
    rewritten.append(contents.substr(copiedCursor));
    return rewritten;
}

[[nodiscard]] std::optional<std::map<std::string, std::string, std::less<>>> BuildReferenceReplacements(
    const std::filesystem::path& projectRoot,
    const TreeSnapshot& tree,
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const bool deleting)
{
    std::map<std::string, std::string, std::less<>> replacements;
    for (const auto& file : tree.files)
    {
        const auto beforePath = SnapshotFilePath(tree, source, file);
        // A .meta companion is not an asset reference target. It remains in the
        // manifest for fingerprinting and movement, but never changes object
        // graph tokens.
        if (beforePath.extension() == ".meta")
            continue;

        const auto afterPath = deleting
            ? std::filesystem::path {}
            : TranslateTreePath(tree, source, destination, beforePath);
        const auto beforeAliases = ReferenceAliases(projectRoot, beforePath);
        const auto afterAliases = deleting
            ? std::vector<std::string> {}
            : ReferenceAliases(projectRoot, afterPath);
        if (!deleting && beforeAliases.size() != afterAliases.size())
            return std::nullopt;

        for (size_t index = 0u; index < beforeAliases.size(); ++index)
        {
            const auto inserted = replacements.emplace(
                beforeAliases[index],
                deleting ? std::string("?") : afterAliases[index]);
            if (!inserted.second && inserted.first->second != (deleting ? std::string("?") : afterAliases[index]))
                return std::nullopt;
        }
    }
    return replacements;
}

[[nodiscard]] std::optional<std::vector<ReferenceFileSnapshot>> CaptureReferenceSnapshots(
    const std::filesystem::path& projectRoot,
    const TreeSnapshot& tree,
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const bool deleting)
{
    const auto replacements = BuildReferenceReplacements(projectRoot, tree, source, destination, deleting);
    if (!replacements.has_value())
        return std::nullopt;
    if (replacements->empty())
        return std::vector<ReferenceFileSnapshot> {};

    const auto assetsRoot = Normalize(projectRoot / "Assets");
    std::error_code error;
    if (!std::filesystem::is_directory(assetsRoot, error) || error)
        return std::nullopt;

    std::vector<ReferenceFileSnapshot> snapshots;
    for (std::filesystem::recursive_directory_iterator iterator(
             assetsRoot,
             std::filesystem::directory_options::skip_permission_denied,
             error),
         end;
         iterator != end && !error;
         iterator.increment(error))
    {
        const auto& entry = *iterator;
        if (entry.is_directory(error) && !error)
            continue;
        error.clear();
        if (!entry.is_regular_file(error) || error)
            return std::nullopt;
        const auto beforePath = Normalize(entry.path());
        // Metadata carries the stable AssetId, not serialized PPtr paths.
        if (beforePath.extension() == ".meta")
            continue;
        // A deleted tree disappears entirely. Snapshot only durable files that
        // remain after the command.
        if (deleting && IsPathInTree(tree, source, beforePath))
            continue;

        const auto beforeContents = ReadContents(beforePath);
        if (!beforeContents.has_value())
            return std::nullopt;
        const auto afterContents = RewriteReferenceTokens(*beforeContents, *replacements);
        if (afterContents == *beforeContents)
            continue;

        ReferenceFileSnapshot snapshot;
        snapshot.beforePath = beforePath;
        snapshot.afterPath = IsPathInTree(tree, source, beforePath)
            ? TranslateTreePath(tree, source, destination, beforePath)
            : beforePath;
        snapshot.beforeContents = *beforeContents;
        snapshot.afterContents = std::move(afterContents);
        snapshot.beforeFingerprint = MakeFingerprint(snapshot.beforeContents);
        snapshot.afterFingerprint = MakeFingerprint(snapshot.afterContents);
        snapshots.push_back(std::move(snapshot));
    }
    if (error)
        return std::nullopt;
    return snapshots;
}

[[nodiscard]] TreeSnapshot BuildAfterTreeSnapshot(
    TreeSnapshot before,
    const std::filesystem::path& source,
    const std::vector<ReferenceFileSnapshot>& references)
{
    for (auto& file : before.files)
    {
        const auto path = SnapshotFilePath(before, source, file);
        const auto found = std::find_if(
            references.begin(),
            references.end(),
            [&path](const ReferenceFileSnapshot& reference)
            {
                return reference.beforePath == path;
            });
        if (found == references.end())
            continue;
        file.contents = found->afterContents;
        file.fingerprint = found->afterFingerprint;
    }
    return before;
}

[[nodiscard]] bool ReferenceSnapshotsMatch(
    const std::vector<ReferenceFileSnapshot>& references,
    const bool after,
    const bool useAfterPaths)
{
    for (const auto& reference : references)
    {
        const auto& path = useAfterPaths ? reference.afterPath : reference.beforePath;
        const auto fingerprint = ReadFingerprint(path);
        const auto& expected = after ? reference.afterFingerprint : reference.beforeFingerprint;
        if (!fingerprint.has_value() || *fingerprint != expected)
            return false;
    }
    return true;
}

[[nodiscard]] bool WriteReferenceSnapshots(
    const std::vector<ReferenceFileSnapshot>& references,
    const bool after,
    const bool useAfterPaths,
    bool& rollbackFailed)
{
    for (const auto& reference : references)
    {
        const auto& path = useAfterPaths ? reference.afterPath : reference.beforePath;
        const auto& contents = after ? reference.afterContents : reference.beforeContents;
        if (WriteContentsAtomically(path, contents))
            continue;

        // The caller owns the broader transaction rollback. Report a failed
        // reference write as recoverable until that rollback itself fails.
        rollbackFailed = false;
        return false;
    }
    return true;
}

[[nodiscard]] bool MoveTree(
    const TreeSnapshot& expected,
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    bool& rollbackFailed)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("moving an asset tree");
    if (!TreeMatches(expected, source) || !TreeDestinationIsVacant(destination))
        return false;

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
        return false;

    if (expected.rootMeta.has_value())
    {
        std::filesystem::rename(MetaPathFor(source), MetaPathFor(destination), error);
        if (error)
            return false;
    }

    std::filesystem::rename(source, destination, error);
    if (!error)
        return true;

    if (expected.rootMeta.has_value())
    {
        std::error_code restoreError;
        std::filesystem::rename(MetaPathFor(destination), MetaPathFor(source), restoreError);
        rollbackFailed = static_cast<bool>(restoreError);
    }
    return false;
}

[[nodiscard]] bool HasExistingParentDirectory(const std::filesystem::path& path)
{
    std::error_code error;
    return !path.parent_path().empty() &&
        std::filesystem::is_directory(path.parent_path(), error) && !error;
}

[[nodiscard]] bool DestinationIsVacant(const std::filesystem::path& path)
{
    std::error_code error;
    const bool sourceExists = std::filesystem::exists(path, error);
    if (error || sourceExists)
        return false;

    error.clear();
    const bool metaExists = std::filesystem::exists(MetaPathFor(path), error);
    return !error && !metaExists;
}

[[nodiscard]] std::optional<AssetFileBundle> BuildBundle(const std::filesystem::path& source)
{
    const auto sourceFingerprint = ReadFingerprint(source);
    if (!sourceFingerprint.has_value())
        return std::nullopt;

    AssetFileBundle bundle;
    bundle.source = source;
    bundle.meta = MetaPathFor(source);
    bundle.sourceFingerprint = *sourceFingerprint;
    if (std::filesystem::exists(bundle.meta))
    {
        bundle.metaFingerprint = ReadFingerprint(bundle.meta);
        if (!bundle.metaFingerprint.has_value())
            return std::nullopt;
    }
    return bundle;
}

[[nodiscard]] bool BundleMatches(const AssetFileBundle& bundle, const std::filesystem::path& source)
{
    const auto sourceFingerprint = ReadFingerprint(source);
    if (!sourceFingerprint.has_value() || *sourceFingerprint != bundle.sourceFingerprint)
        return false;

    const auto metaPath = MetaPathFor(source);
    if (!bundle.metaFingerprint.has_value())
        return !std::filesystem::exists(metaPath);

    const auto metaFingerprint = ReadFingerprint(metaPath);
    return metaFingerprint.has_value() && *metaFingerprint == *bundle.metaFingerprint;
}

[[nodiscard]] bool CopySourceFile(const AssetFileBundle& bundle, const std::filesystem::path& destination)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("copying an asset bundle");
    std::error_code error;
    if (!BundleMatches(bundle, bundle.source) || std::filesystem::exists(destination) ||
        std::filesystem::exists(MetaPathFor(destination)))
    {
        return false;
    }

    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
        return false;

    std::filesystem::copy_file(bundle.source, destination, std::filesystem::copy_options::none, error);
    if (error)
        return false;

    return true;
}

[[nodiscard]] bool MoveBundle(
    const AssetFileBundle& bundle,
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    bool& rollbackFailed)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("moving an asset bundle");
    std::error_code error;
    if (!BundleMatches(bundle, source) || std::filesystem::exists(destination) ||
        std::filesystem::exists(MetaPathFor(destination)))
    {
        return false;
    }

    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
        return false;

    const bool hasMeta = bundle.metaFingerprint.has_value();
    if (hasMeta)
    {
        std::filesystem::rename(MetaPathFor(source), MetaPathFor(destination), error);
        if (error)
            return false;
    }

    std::filesystem::rename(source, destination, error);
    if (!error)
        return true;

    if (hasMeta)
    {
        std::error_code restoreError;
        std::filesystem::rename(MetaPathFor(destination), MetaPathFor(source), restoreError);
        rollbackFailed = static_cast<bool>(restoreError);
    }
    return false;
}

[[nodiscard]] bool MoveBundleToStaging(
    const std::filesystem::path& destination,
    const bool hasMeta,
    const std::filesystem::path& stagingDirectory,
    bool& rollbackFailed)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("moving an asset bundle to UndoTrash");
    std::error_code error;
    std::filesystem::create_directories(stagingDirectory, error);
    if (error)
        return false;

    const auto stagedSource = stagingDirectory / destination.filename();
    const auto stagedMeta = stagingDirectory / MetaPathFor(destination).filename();
    if (std::filesystem::exists(stagedSource) || std::filesystem::exists(stagedMeta))
        return false;

    if (hasMeta)
    {
        std::filesystem::rename(MetaPathFor(destination), stagedMeta, error);
        if (error)
            return false;
    }

    std::filesystem::rename(destination, stagedSource, error);
    if (!error)
        return true;

    if (hasMeta)
    {
        std::error_code restoreError;
        std::filesystem::rename(stagedMeta, MetaPathFor(destination), restoreError);
        rollbackFailed = static_cast<bool>(restoreError);
    }
    return false;
}

[[nodiscard]] bool WriteCreatedFile(
    const std::filesystem::path& destination,
    const std::string& contents,
    const NLS::Core::Assets::AssetMeta& metadata,
    AssetFileBundle& createdBundle,
    bool& rollbackFailed)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("creating an asset bundle");
    if (!HasExistingParentDirectory(destination) || !DestinationIsVacant(destination))
        return false;

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    if (!contents.empty())
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output)
    {
        std::error_code error;
        std::filesystem::remove(destination, error);
        rollbackFailed = static_cast<bool>(error);
        return false;
    }

    const auto metaPath = MetaPathFor(destination);
    if (!metadata.Save(metaPath))
    {
        std::error_code error;
        std::filesystem::remove(metaPath, error);
        rollbackFailed = static_cast<bool>(error);
        error.clear();
        std::filesystem::remove(destination, error);
        rollbackFailed = rollbackFailed || static_cast<bool>(error);
        return false;
    }

    const auto bundle = BuildBundle(destination);
    if (!bundle.has_value() || !bundle->metaFingerprint.has_value())
    {
        std::error_code error;
        std::filesystem::remove(metaPath, error);
        rollbackFailed = static_cast<bool>(error);
        error.clear();
        std::filesystem::remove(destination, error);
        rollbackFailed = rollbackFailed || static_cast<bool>(error);
        return false;
    }

    createdBundle = *bundle;
    return true;
}

[[nodiscard]] std::string ResourcePathForProjectAsset(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& assetPath)
{
    if (projectRoot.empty() || assetPath.empty())
        return {};

    const auto assetsRoot = Normalize(projectRoot / "Assets");
    const auto relative = Normalize(assetPath).lexically_relative(assetsRoot);
    if (relative.empty())
        return {};
    for (const auto& segment : relative)
    {
        if (segment == "..")
            return {};
    }

    const auto path = relative.generic_string();
    return path == "." ? std::string("Assets") : "Assets/" + path;
}

[[nodiscard]] NLS::Core::Assets::AssetId AssetIdForPath(const std::filesystem::path& assetPath)
{
    if (assetPath.empty())
        return {};

    const auto metadata = NLS::Core::Assets::AssetMeta::Load(MetaPathFor(assetPath));
    return metadata.has_value() ? metadata->id : NLS::Core::Assets::AssetId {};
}

[[nodiscard]] NLS::Core::Assets::AssetId AssetIdFromIdentifier(const std::string_view identifier)
{
    const auto guid = NLS::Guid::TryParse(identifier);
    return guid.has_value() ? NLS::Core::Assets::AssetId(*guid) : NLS::Core::Assets::AssetId {};
}

void AddUniqueScope(UndoCommandInfo& info, UndoScopeId scope)
{
    if (!scope.IsValid() || std::find(info.scopes.begin(), info.scopes.end(), scope) != info.scopes.end())
        return;
    info.scopes.push_back(std::move(scope));
}

void AddProjectAssetScope(
    UndoCommandInfo& info,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& assetPath,
    NLS::Core::Assets::AssetId expectedAssetId = {})
{
    if (assetPath.empty())
        return;

    if (!expectedAssetId.IsValid())
        expectedAssetId = AssetIdForPath(assetPath);
    const auto scope = MakeAssetUndoScope(
        ResourcePathForProjectAsset(projectRoot, assetPath),
        expectedAssetId.IsValid() ? expectedAssetId.ToString() : std::string {});
    AddUniqueScope(info, scope);
}

void AddProjectAssetPathScopes(
    UndoCommandInfo& info,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    NLS::Core::Assets::AssetId assetId = {})
{
    if (!assetId.IsValid())
        assetId = AssetIdForPath(source);
    AddProjectAssetScope(info, projectRoot, source, assetId);
    if (!destination.empty())
        AddProjectAssetScope(info, projectRoot, destination, assetId);
}

[[nodiscard]] UndoCommandInfo BuildInfo(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::string& assetIdentifier,
    const AssetFileBundle& bundle,
    const NLS::Core::Assets::AssetId destinationAssetId)
{
    UndoCommandInfo info;
    info.label = "Duplicate Asset";
    info.sourceModule = "Asset Browser";
    // The source is read-only. Only the newly created asset owns a dirty/save
    // scope, so an external source edit cannot make an unrelated destination
    // appear dirty.
    AddProjectAssetScope(info, projectRoot, destination, destinationAssetId);
    info.writeThroughScopes = info.scopes;
    info.affected = {
        assetIdentifier.empty() ? Normalize(source).generic_string() : assetIdentifier,
        Normalize(destination).generic_string()};
    info.estimatedBytes = static_cast<size_t>(bundle.sourceFingerprint.size) +
        (bundle.metaFingerprint.has_value() ? static_cast<size_t>(bundle.metaFingerprint->size) : 0u);
    return info;
}

class CreateAssetFileCommand final : public IEditorUndoCommand
{
public:
    CreateAssetFileCommand(
        UndoCommandInfo info,
        std::filesystem::path projectRoot,
        std::filesystem::path destination,
        std::string contents,
        NLS::Core::Assets::AssetMeta metadata)
        : m_info(std::move(info))
        , m_projectRoot(Normalize(projectRoot))
        , m_destination(Normalize(destination))
        , m_contents(std::move(contents))
        , m_metadata(std::move(metadata))
    {
    }

    ~CreateAssetFileCommand() override
    {
        if (m_commandTrashRoot.empty())
            return;
        std::error_code error;
        std::filesystem::remove_all(m_commandTrashRoot, error);
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext&) const override
    {
        if (!HasExistingParentDirectory(m_destination))
            return UndoValidationResult::Conflict("The target asset folder no longer exists.");

        if (direction == UndoDirection::Redo)
        {
            if (!DestinationIsVacant(m_destination))
                return UndoValidationResult::Conflict("The created asset path is already occupied.");
            if (m_stagedSource.empty())
                return UndoValidationResult::Ready();
            return m_createdBundle.has_value() && BundleMatches(*m_createdBundle, m_stagedSource)
                ? UndoValidationResult::Ready()
                : UndoValidationResult::Conflict("The created asset changed outside the Undo timeline.");
        }

        return m_createdBundle.has_value() && BundleMatches(*m_createdBundle, m_destination)
            ? UndoValidationResult::Ready()
            : UndoValidationResult::Conflict("The created asset changed outside the Undo timeline.");
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext& context,
        UndoCompletion) override
    {
        bool rollbackFailed = false;
        if (direction == UndoDirection::Redo)
        {
            if (!DestinationIsVacant(m_destination))
                return UndoStartResult::Failed("The created asset path is already occupied.");

            if (!m_stagedSource.empty())
            {
                if (!m_createdBundle.has_value() || !BundleMatches(*m_createdBundle, m_stagedSource))
                {
                    return UndoStartResult::Failed(
                        "The created asset changed outside the Undo timeline.");
                }
                return MoveBundle(*m_createdBundle, m_stagedSource, m_destination, rollbackFailed)
                    ? UndoStartResult::Completed()
                    : UndoStartResult::Failed(
                        "The created asset bundle could not be restored atomically.",
                        rollbackFailed);
            }

            AssetFileBundle createdBundle;
            if (!WriteCreatedFile(m_destination, m_contents, m_metadata, createdBundle, rollbackFailed))
            {
                return UndoStartResult::Failed(
                    "The asset bundle could not be created atomically.",
                    rollbackFailed);
            }
            m_createdBundle = std::move(createdBundle);
            return UndoStartResult::Completed();
        }

        if (!m_createdBundle.has_value() || !BundleMatches(*m_createdBundle, m_destination))
            return UndoStartResult::Failed("The created asset changed outside the Undo timeline.");

        if (m_commandTrashRoot.empty())
        {
            m_commandTrashRoot = m_projectRoot / "Library" / "Editor" / "UndoTrash" /
                "asset-file" / std::to_string(context.commandId);
        }
        const auto stagingDirectory = m_commandTrashRoot / "create" / std::to_string(++m_removalGeneration);
        if (!MoveBundleToStaging(
                m_destination,
                m_createdBundle->metaFingerprint.has_value(),
                stagingDirectory,
                rollbackFailed))
        {
            return UndoStartResult::Failed(
                "The created asset bundle could not be moved to UndoTrash.",
                rollbackFailed);
        }
        m_stagedSource = stagingDirectory / m_destination.filename();
        return UndoStartResult::Completed();
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        return false;
    }

private:
    UndoCommandInfo m_info;
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_destination;
    std::string m_contents;
    NLS::Core::Assets::AssetMeta m_metadata;
    std::optional<AssetFileBundle> m_createdBundle;
    std::filesystem::path m_commandTrashRoot;
    std::filesystem::path m_stagedSource;
    uint64_t m_removalGeneration = 0u;
};

class CreateAssetDirectoryCommand final : public IEditorUndoCommand
{
public:
    CreateAssetDirectoryCommand(UndoCommandInfo info, std::filesystem::path directory)
        : m_info(std::move(info))
        , m_directory(Normalize(directory))
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext&) const override
    {
        if (direction == UndoDirection::Redo)
        {
            if (!HasExistingParentDirectory(m_directory))
                return UndoValidationResult::Conflict("The target asset folder no longer exists.");
            return !std::filesystem::exists(m_directory)
                ? UndoValidationResult::Ready()
                : UndoValidationResult::Conflict("The created folder path is already occupied.");
        }

        std::error_code error;
        if (!std::filesystem::is_directory(m_directory, error) || error)
            return UndoValidationResult::Conflict("The created folder no longer exists.");
        const bool empty = std::filesystem::is_empty(m_directory, error);
        return !error && empty
            ? UndoValidationResult::Ready()
            : UndoValidationResult::Conflict(
                "The created folder contains content and cannot be removed safely.");
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext&,
        UndoCompletion) override
    {
        NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("creating or removing an asset folder");
        std::error_code error;
        if (direction == UndoDirection::Redo)
        {
            if (!HasExistingParentDirectory(m_directory) || std::filesystem::exists(m_directory))
                return UndoStartResult::Failed("The created folder path is already occupied.");
            return std::filesystem::create_directory(m_directory, error) && !error
                ? UndoStartResult::Completed()
                : UndoStartResult::Failed("The asset folder could not be created.");
        }

        if (!std::filesystem::is_directory(m_directory, error) || error)
            return UndoStartResult::Failed("The created folder no longer exists.");
        error.clear();
        if (!std::filesystem::is_empty(m_directory, error) || error)
        {
            return UndoStartResult::Failed(
                "The created folder contains content and cannot be removed safely.");
        }
        return std::filesystem::remove(m_directory, error) && !error
            ? UndoStartResult::Completed()
            : UndoStartResult::Failed("The created folder could not be removed.");
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        return false;
    }

private:
    UndoCommandInfo m_info;
    std::filesystem::path m_directory;
};

class DuplicateAssetFileCommand final : public IEditorUndoCommand
{
public:
    DuplicateAssetFileCommand(
        UndoCommandInfo info,
        AssetFileBundle bundle,
        std::filesystem::path projectRoot,
        std::filesystem::path destination,
        NLS::Core::Assets::AssetId duplicateAssetId)
        : m_info(std::move(info))
        , m_bundle(std::move(bundle))
        , m_projectRoot(Normalize(projectRoot))
        , m_destination(Normalize(destination))
        , m_duplicateAssetId(std::move(duplicateAssetId))
    {
    }

    ~DuplicateAssetFileCommand() override
    {
        if (m_commandTrashRoot.empty())
            return;
        std::error_code error;
        std::filesystem::remove_all(m_commandTrashRoot, error);
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext&) const override
    {
        if (!BundleMatches(m_bundle, m_bundle.source))
            return UndoValidationResult::Conflict("The source asset changed outside the Undo timeline.");

        if (direction == UndoDirection::Redo)
        {
            return !std::filesystem::exists(m_destination) && !std::filesystem::exists(MetaPathFor(m_destination))
                ? UndoValidationResult::Ready()
                : UndoValidationResult::Conflict("The duplicate destination is already occupied.");
        }

        AssetFileBundle destinationBundle = m_bundle;
        destinationBundle.source = m_destination;
        destinationBundle.meta = MetaPathFor(m_destination);
        destinationBundle.metaFingerprint = m_destinationMetaFingerprint;
        return BundleMatches(destinationBundle, m_destination)
            ? UndoValidationResult::Ready()
            : UndoValidationResult::Conflict("The duplicated asset changed outside the Undo timeline.");
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext& context,
        UndoCompletion) override
    {
        bool applied = false;
        bool rollbackFailed = false;
        if (direction == UndoDirection::Redo)
        {
            applied = CopySourceFile(m_bundle, m_destination);
            if (applied)
            {
                auto meta = NLS::Core::Assets::AssetMeta::Load(m_bundle.meta)
                    .value_or(NLS::Core::Assets::AssetMeta::CreateForAsset(m_destination));
                meta.id = m_duplicateAssetId;
                meta.assetType = NLS::Core::Assets::InferAssetType(m_destination);
                meta.importerId = NLS::Core::Assets::InferImporterId(meta.assetType);
                if (!meta.Save(MetaPathFor(m_destination)))
                {
                    std::error_code error;
                    std::filesystem::remove(MetaPathFor(m_destination), error);
                    rollbackFailed = static_cast<bool>(error);
                    error.clear();
                    std::filesystem::remove(m_destination, error);
                    rollbackFailed = rollbackFailed || static_cast<bool>(error);
                    applied = false;
                }
                else
                {
                    m_destinationMetaFingerprint = ReadFingerprint(MetaPathFor(m_destination));
                    applied = m_destinationMetaFingerprint.has_value();
                }
            }
        }
        else
        {
            AssetFileBundle destinationBundle = m_bundle;
            destinationBundle.source = m_destination;
            destinationBundle.meta = MetaPathFor(m_destination);
            destinationBundle.metaFingerprint = m_destinationMetaFingerprint;
            if (!BundleMatches(destinationBundle, m_destination))
                return UndoStartResult::Failed("The duplicated asset changed outside the Undo timeline.");

            if (m_commandTrashRoot.empty())
            {
                m_commandTrashRoot = m_projectRoot / "Library" / "Editor" / "UndoTrash" /
                    "asset-file" / std::to_string(context.commandId);
            }
            const auto stagingDirectory = m_commandTrashRoot / std::to_string(++m_removalGeneration);
            applied = MoveBundleToStaging(
                m_destination,
                m_destinationMetaFingerprint.has_value(),
                stagingDirectory,
                rollbackFailed);
        }
        return applied
            ? UndoStartResult::Completed()
            : UndoStartResult::Failed("The asset bundle could not be restored atomically.", rollbackFailed);
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        return false;
    }

private:
    UndoCommandInfo m_info;
    AssetFileBundle m_bundle;
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_destination;
    NLS::Core::Assets::AssetId m_duplicateAssetId;
    std::optional<FileFingerprint> m_destinationMetaFingerprint;
    std::filesystem::path m_commandTrashRoot;
    uint64_t m_removalGeneration = 0u;
};

class RenameAssetFileCommand final : public IEditorUndoCommand
{
public:
    RenameAssetFileCommand(
        UndoCommandInfo info,
        AssetFileBundle sourceBundle,
        std::filesystem::path source,
        std::filesystem::path destination)
        : m_info(std::move(info))
        , m_sourceBundle(std::move(sourceBundle))
        , m_source(Normalize(source))
        , m_destination(Normalize(destination))
    {
        m_destinationBundle = m_sourceBundle;
        m_destinationBundle.source = m_destination;
        m_destinationBundle.meta = MetaPathFor(m_destination);
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext&) const override
    {
        const auto& currentBundle = direction == UndoDirection::Redo
            ? m_sourceBundle
            : m_destinationBundle;
        const auto& currentPath = direction == UndoDirection::Redo
            ? m_source
            : m_destination;
        const auto& targetPath = direction == UndoDirection::Redo
            ? m_destination
            : m_source;

        if (!BundleMatches(currentBundle, currentPath))
        {
            return UndoValidationResult::Conflict(
                "The asset bundle changed outside the Undo timeline.");
        }
        return !std::filesystem::exists(targetPath) && !std::filesystem::exists(MetaPathFor(targetPath))
            ? UndoValidationResult::Ready()
            : UndoValidationResult::Conflict("The asset rename destination is already occupied.");
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext&,
        UndoCompletion) override
    {
        bool rollbackFailed = false;
        const auto& sourceBundle = direction == UndoDirection::Redo
            ? m_sourceBundle
            : m_destinationBundle;
        const auto& sourcePath = direction == UndoDirection::Redo
            ? m_source
            : m_destination;
        const auto& destinationPath = direction == UndoDirection::Redo
            ? m_destination
            : m_source;
        return MoveBundle(sourceBundle, sourcePath, destinationPath, rollbackFailed)
            ? UndoStartResult::Completed()
            : UndoStartResult::Failed("The asset bundle could not be renamed atomically.", rollbackFailed);
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        return false;
    }

private:
    UndoCommandInfo m_info;
    AssetFileBundle m_sourceBundle;
    AssetFileBundle m_destinationBundle;
    std::filesystem::path m_source;
    std::filesystem::path m_destination;
};

class DeleteAssetFileCommand final : public IEditorUndoCommand
{
public:
    DeleteAssetFileCommand(
        UndoCommandInfo info,
        AssetFileBundle bundle,
        std::filesystem::path projectRoot,
        std::filesystem::path source)
        : m_info(std::move(info))
        , m_bundle(std::move(bundle))
        , m_projectRoot(Normalize(projectRoot))
        , m_source(Normalize(source))
    {
    }

    ~DeleteAssetFileCommand() override
    {
        if (m_commandTrashRoot.empty())
            return;
        std::error_code error;
        std::filesystem::remove_all(m_commandTrashRoot, error);
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext&) const override
    {
        if (direction == UndoDirection::Redo)
        {
            return BundleMatches(m_bundle, m_source)
                ? UndoValidationResult::Ready()
                : UndoValidationResult::Conflict("The asset changed outside the Undo timeline.");
        }

        if (m_stagedSource.empty() || !BundleMatches(m_bundle, m_stagedSource))
            return UndoValidationResult::Conflict("The deleted asset changed outside the Undo timeline.");
        return !std::filesystem::exists(m_source) && !std::filesystem::exists(MetaPathFor(m_source))
            ? UndoValidationResult::Ready()
            : UndoValidationResult::Conflict("The deleted asset path is already occupied.");
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext& context,
        UndoCompletion) override
    {
        bool rollbackFailed = false;
        if (direction == UndoDirection::Redo)
        {
            if (!BundleMatches(m_bundle, m_source))
                return UndoStartResult::Failed("The asset changed outside the Undo timeline.");

            if (m_commandTrashRoot.empty())
            {
                m_commandTrashRoot = m_projectRoot / "Library" / "Editor" / "UndoTrash" /
                    "asset-file" / std::to_string(context.commandId);
            }

            const auto stagingDirectory = m_commandTrashRoot / std::to_string(++m_removalGeneration);
            if (!MoveBundleToStaging(
                    m_source,
                    m_bundle.metaFingerprint.has_value(),
                    stagingDirectory,
                    rollbackFailed))
            {
                return UndoStartResult::Failed("The asset bundle could not be moved to UndoTrash.", rollbackFailed);
            }
            m_stagedSource = stagingDirectory / m_source.filename();
            return UndoStartResult::Completed();
        }

        if (m_stagedSource.empty() || !BundleMatches(m_bundle, m_stagedSource) ||
            std::filesystem::exists(m_source) || std::filesystem::exists(MetaPathFor(m_source)))
        {
            return UndoStartResult::Failed("The deleted asset cannot be restored safely.");
        }
        return MoveBundle(m_bundle, m_stagedSource, m_source, rollbackFailed)
            ? UndoStartResult::Completed()
            : UndoStartResult::Failed("The deleted asset bundle could not be restored atomically.", rollbackFailed);
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        return false;
    }

private:
    UndoCommandInfo m_info;
    AssetFileBundle m_bundle;
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_source;
    std::filesystem::path m_commandTrashRoot;
    std::filesystem::path m_stagedSource;
    uint64_t m_removalGeneration = 0u;
};

class ProjectAssetMutationCommand final : public IEditorUndoCommand
{
public:
    ProjectAssetMutationCommand(
        UndoCommandInfo info,
        std::filesystem::path projectRoot,
        std::filesystem::path source,
        std::filesystem::path destination,
        TreeSnapshot beforeTree,
        TreeSnapshot afterTree,
        std::vector<ReferenceFileSnapshot> references,
        const bool deleting)
        : m_info(std::move(info))
        , m_projectRoot(Normalize(projectRoot))
        , m_source(Normalize(source))
        , m_destination(Normalize(destination))
        , m_beforeTree(std::move(beforeTree))
        , m_afterTree(std::move(afterTree))
        , m_references(std::move(references))
        , m_deleting(deleting)
    {
    }

    ~ProjectAssetMutationCommand() override
    {
        if (m_trashRoot.empty())
            return;
        std::error_code error;
        std::filesystem::remove_all(m_trashRoot, error);
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext&) const override
    {
        if (direction == UndoDirection::Redo)
        {
            if (!TreeMatches(m_beforeTree, m_source))
            {
                return UndoValidationResult::Conflict(
                    "The source asset tree changed outside the Undo timeline.");
            }
            const auto& destination = m_deleting ? m_stagedRoot : m_destination;
            if (!destination.empty() && !TreeDestinationIsVacant(destination))
            {
                return UndoValidationResult::Conflict(
                    "The asset mutation destination is already occupied.");
            }
            return ReferenceSnapshotsMatch(m_references, false, false)
                ? UndoValidationResult::Ready()
                : UndoValidationResult::Conflict(
                    "A serialized asset reference changed outside the Undo timeline.");
        }

        const auto& currentRoot = m_deleting ? m_stagedRoot : m_destination;
        const auto& expectedTree = m_deleting ? m_beforeTree : m_afterTree;
        if (currentRoot.empty() || !TreeMatches(expectedTree, currentRoot))
        {
            return UndoValidationResult::Conflict(
                "The mutated asset tree changed outside the Undo timeline.");
        }
        if (!TreeDestinationIsVacant(m_source))
        {
            return UndoValidationResult::Conflict(
                "The original asset path is already occupied.");
        }
        const bool referencePathsArePostMutation = !m_deleting;
        return ReferenceSnapshotsMatch(m_references, true, referencePathsArePostMutation)
            ? UndoValidationResult::Ready()
            : UndoValidationResult::Conflict(
                "A serialized asset reference changed outside the Undo timeline.");
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext& context,
        UndoCompletion) override
    {
        if (m_deleting && direction == UndoDirection::Redo && !EnsureTrashRoot(context))
            return UndoStartResult::Failed("The UndoTrash staging path could not be prepared.");

        return direction == UndoDirection::Redo
            ? ApplyRedo()
            : ApplyUndo();
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        return false;
    }

private:
    [[nodiscard]] bool EnsureTrashRoot(const UndoExecutionContext& context)
    {
        if (!m_stagedRoot.empty())
            return TreeDestinationIsVacant(m_stagedRoot);

        if (context.commandId == 0u)
            return false;
        m_trashRoot = m_projectRoot / "Library" / "Editor" / "UndoTrash" /
            "asset-tree" / std::to_string(context.commandId);
        m_stagedRoot = m_trashRoot / "payload";
        return TreeDestinationIsVacant(m_stagedRoot);
    }

    [[nodiscard]] UndoStartResult ApplyRedo()
    {
        const auto& destination = m_deleting ? m_stagedRoot : m_destination;
        bool rollbackFailed = false;
        if (!MoveTree(m_beforeTree, m_source, destination, rollbackFailed))
        {
            return UndoStartResult::Failed(
                m_deleting ? "The asset tree could not be moved to UndoTrash."
                           : "The asset tree could not be moved atomically.",
                rollbackFailed);
        }

        const bool usePostPaths = !m_deleting;
        if (WriteReferenceSnapshots(m_references, true, usePostPaths, rollbackFailed))
            return UndoStartResult::Completed();

        bool referenceRollbackFailed = false;
        const bool referencesRestored = WriteReferenceSnapshots(
            m_references,
            false,
            usePostPaths,
            referenceRollbackFailed);
        bool treeRollbackFailed = false;
        const bool treeRestored = MoveTree(m_beforeTree, destination, m_source, treeRollbackFailed);
        return UndoStartResult::Failed(
            "The serialized references could not be updated; the asset mutation was rolled back.",
            rollbackFailed || referenceRollbackFailed || treeRollbackFailed || !referencesRestored || !treeRestored);
    }

    [[nodiscard]] UndoStartResult ApplyUndo()
    {
        const auto& currentRoot = m_deleting ? m_stagedRoot : m_destination;
        const auto& expectedTree = m_deleting ? m_beforeTree : m_afterTree;
        bool rollbackFailed = false;
        // Move while the tree still has its post-mutation contents. Internal
        // reference snapshots are addressed by the destination path and must
        // be restored only after that move has made their original paths live.
        if (!MoveTree(expectedTree, currentRoot, m_source, rollbackFailed))
        {
            return UndoStartResult::Failed(
                "The asset tree could not be restored atomically.",
                rollbackFailed);
        }

        if (WriteReferenceSnapshots(m_references, false, false, rollbackFailed))
            return UndoStartResult::Completed();

        bool referenceRollbackFailed = false;
        const bool referencesRestored = WriteReferenceSnapshots(
            m_references,
            true,
            false,
            referenceRollbackFailed);
        bool treeRollbackFailed = false;
        const bool treeRestored = referencesRestored && MoveTree(
            expectedTree,
            m_source,
            currentRoot,
            treeRollbackFailed);
        return UndoStartResult::Failed(
            "The serialized references could not be restored; the asset tree was rolled back.",
            rollbackFailed || referenceRollbackFailed || treeRollbackFailed ||
                !referencesRestored || !treeRestored);
    }

    UndoCommandInfo m_info;
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_source;
    std::filesystem::path m_destination;
    TreeSnapshot m_beforeTree;
    TreeSnapshot m_afterTree;
    std::vector<ReferenceFileSnapshot> m_references;
    bool m_deleting = false;
    std::filesystem::path m_trashRoot;
    std::filesystem::path m_stagedRoot;
};

void AddMutationScope(
    UndoCommandInfo& info,
    const UndoScopeKind kind,
    const std::filesystem::path& stablePath)
{
    if (stablePath.empty())
        return;
    AddUniqueScope(info, {kind, Normalize(stablePath).generic_string(), 0u});
}

void AddMutationAffected(UndoCommandInfo& info, const std::filesystem::path& path)
{
    if (path.empty())
        return;
    const auto normalized = Normalize(path).generic_string();
    if (std::find(info.affected.begin(), info.affected.end(), normalized) == info.affected.end())
        info.affected.push_back(normalized);
    if (path.extension() == ".scene")
        AddMutationScope(info, UndoScopeKind::Scene, path);
}

[[nodiscard]] UndoCommandInfo BuildProjectAssetMutationInfo(
    std::string label,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string assetIdentifier,
    const TreeSnapshot& beforeTree,
    const TreeSnapshot& afterTree,
    const std::vector<ReferenceFileSnapshot>& references)
{
    UndoCommandInfo info;
    info.label = label.empty()
        ? (destination.empty() ? "Delete Asset Tree" : "Move Asset Tree")
        : std::move(label);
    info.sourceModule = "Asset Browser";
    const auto explicitAssetId = AssetIdFromIdentifier(assetIdentifier);
    AddProjectAssetPathScopes(info, projectRoot, source, destination, explicitAssetId);
    if (!assetIdentifier.empty())
        info.affected.push_back(std::move(assetIdentifier));
    AddMutationAffected(info, source);
    AddMutationAffected(info, destination);

    size_t estimatedBytes = 0u;
    for (const auto& file : beforeTree.files)
    {
        estimatedBytes += file.contents.size();
        const auto beforePath = SnapshotFilePath(beforeTree, source, file);
        AddMutationAffected(info, beforePath);
        if (beforePath.extension() == ".meta")
            continue;

        const auto fileAssetId = AssetIdForPath(beforePath);
        AddProjectAssetPathScopes(
            info,
            projectRoot,
            beforePath,
            destination.empty() ? std::filesystem::path {} :
                TranslateTreePath(beforeTree, source, destination, beforePath),
            fileAssetId);
    }
    for (const auto& file : afterTree.files)
        estimatedBytes += file.contents.size();
    if (beforeTree.rootMeta.has_value())
        estimatedBytes += beforeTree.rootMeta->contents.size();
    if (afterTree.rootMeta.has_value())
        estimatedBytes += afterTree.rootMeta->contents.size();
    for (const auto& reference : references)
    {
        estimatedBytes += reference.beforeContents.size() + reference.afterContents.size();
        AddMutationAffected(info, reference.beforePath);
        AddMutationAffected(info, reference.afterPath);
        const auto referenceAssetId = AssetIdForPath(reference.beforePath);
        AddProjectAssetPathScopes(
            info,
            projectRoot,
            reference.beforePath,
            reference.afterPath,
            referenceAssetId);
    }
    info.estimatedBytes = estimatedBytes;
    info.writeThroughScopes = info.scopes;
    return info;
}

[[nodiscard]] UndoCommandInfo BuildFileOperationInfo(
    std::string label,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string assetIdentifier,
    const AssetFileBundle& bundle)
{
    UndoCommandInfo info;
    info.label = std::move(label);
    info.sourceModule = "Asset Browser";
    AddProjectAssetPathScopes(
        info,
        projectRoot,
        source,
        destination,
        AssetIdFromIdentifier(assetIdentifier));
    info.writeThroughScopes = info.scopes;
    info.affected.push_back(assetIdentifier.empty() ? Normalize(source).generic_string() : std::move(assetIdentifier));
    info.affected.push_back(Normalize(source).generic_string());
    if (!destination.empty())
        info.affected.push_back(Normalize(destination).generic_string());
    info.estimatedBytes = static_cast<size_t>(bundle.sourceFingerprint.size) +
        (bundle.metaFingerprint.has_value() ? static_cast<size_t>(bundle.metaFingerprint->size) : 0u);
    return info;
}

[[nodiscard]] UndoCommandInfo BuildCreateOperationInfo(
    std::string label,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& destination,
    std::string assetIdentifier,
    const size_t estimatedBytes,
    const NLS::Core::Assets::AssetId assetId = {})
{
    UndoCommandInfo info;
    info.label = label.empty() ? "Create Asset" : std::move(label);
    info.sourceModule = "Asset Browser";
    AddProjectAssetScope(info, projectRoot, destination, assetId);
    info.writeThroughScopes = info.scopes;
    info.affected.push_back(
        assetIdentifier.empty() ? Normalize(destination).generic_string() : std::move(assetIdentifier));
    info.affected.push_back(Normalize(destination).generic_string());
    info.estimatedBytes = estimatedBytes;
    return info;
}
} // namespace

UndoCommandPtr MakeProjectAssetMutationCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    std::string assetIdentifier,
    std::string label)
{
    const auto project = Normalize(projectRoot);
    const auto assetsRoot = Normalize(project / "Assets");
    const auto source = Normalize(sourcePath);
    const auto destination = Normalize(destinationPath);
    const bool deleting = destination.empty();
    if (project.empty() || source.empty() || !IsDescendantPath(assetsRoot, source) ||
        (!deleting && (!IsDescendantPath(assetsRoot, destination) || source == destination)))
    {
        return {};
    }

    const auto beforeTree = CaptureTreeSnapshot(source);
    if (!beforeTree.has_value())
        return {};
    if (!deleting)
    {
        if (!TreeDestinationIsVacant(destination) ||
            (beforeTree->directory && IsDescendantPath(source, destination)))
        {
            return {};
        }
    }

    const auto references = CaptureReferenceSnapshots(
        project,
        *beforeTree,
        source,
        destination,
        deleting);
    if (!references.has_value())
        return {};

    const auto afterTree = BuildAfterTreeSnapshot(*beforeTree, source, *references);
    auto info = BuildProjectAssetMutationInfo(
        std::move(label),
        project,
        source,
        destination,
        std::move(assetIdentifier),
        *beforeTree,
        afterTree,
        *references);
    return std::make_unique<ProjectAssetMutationCommand>(
        std::move(info),
        project,
        source,
        destination,
        *beforeTree,
        afterTree,
        *references,
        deleting);
}

UndoCommandPtr MakeCreateFileCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& destinationPath,
    std::string contents,
    std::string assetIdentifier,
    std::string label,
    NLS::Core::Assets::AssetId assetId)
{
    const auto destination = Normalize(destinationPath);
    if (projectRoot.empty() || destination.empty() || !HasExistingParentDirectory(destination) ||
        !DestinationIsVacant(destination))
    {
        return {};
    }

    auto metadata = NLS::Core::Assets::AssetMeta::CreateForAsset(destination);
    if (assetId.IsValid())
        metadata.id = assetId;
    return std::make_unique<CreateAssetFileCommand>(
        BuildCreateOperationInfo(
            std::move(label),
            projectRoot,
            destination,
            std::move(assetIdentifier),
            contents.size() + 256u,
            metadata.id),
        projectRoot,
        destination,
        std::move(contents),
        metadata);
}

UndoCommandPtr MakeCreateDirectoryCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& directoryPath,
    std::string assetIdentifier,
    std::string label)
{
    const auto project = Normalize(projectRoot);
    const auto directory = Normalize(directoryPath);
    if (project.empty() || directory.empty() || directory == project ||
        !HasExistingParentDirectory(directory) || std::filesystem::exists(directory))
    {
        return {};
    }

    return std::make_unique<CreateAssetDirectoryCommand>(
        BuildCreateOperationInfo(
            std::move(label),
            project,
            directory,
            std::move(assetIdentifier),
            0u),
        directory);
}

UndoCommandPtr MakeDuplicateCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    std::string assetIdentifier)
{
    const auto source = Normalize(sourcePath);
    const auto destination = Normalize(destinationPath);
    if (projectRoot.empty() || source.empty() || destination.empty() || source == destination)
        return {};

    const auto bundle = BuildBundle(source);
    if (!bundle.has_value())
        return {};

    const auto duplicateAssetId = NLS::Core::Assets::AssetId::New();
    return std::make_unique<DuplicateAssetFileCommand>(
        BuildInfo(projectRoot, source, destination, assetIdentifier, *bundle, duplicateAssetId),
        *bundle,
        projectRoot,
        destination,
        duplicateAssetId);
}

UndoCommandPtr MakeRenameCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    std::string assetIdentifier)
{
    const auto source = Normalize(sourcePath);
    const auto destination = Normalize(destinationPath);
    if (projectRoot.empty() || source.empty() || destination.empty() || source == destination ||
        std::filesystem::exists(destination) || std::filesystem::exists(MetaPathFor(destination)))
    {
        return {};
    }

    const auto bundle = BuildBundle(source);
    if (!bundle.has_value())
        return {};

    return std::make_unique<RenameAssetFileCommand>(
        BuildFileOperationInfo(
            "Rename Asset",
            projectRoot,
            source,
            destination,
            std::move(assetIdentifier),
            *bundle),
        *bundle,
        source,
        destination);
}

UndoCommandPtr MakeDeleteCommand(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    std::string assetIdentifier)
{
    const auto source = Normalize(sourcePath);
    if (projectRoot.empty() || source.empty())
        return {};

    const auto bundle = BuildBundle(source);
    if (!bundle.has_value())
        return {};

    return std::make_unique<DeleteAssetFileCommand>(
        BuildFileOperationInfo(
            "Delete Asset",
            projectRoot,
            source,
            {},
            std::move(assetIdentifier),
            *bundle),
        *bundle,
        projectRoot,
        source);
}
} // namespace NLS::Editor::Undo::AssetFile
