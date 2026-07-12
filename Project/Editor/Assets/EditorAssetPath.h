#pragma once

#include "Assets/AssetPath.h"
#include "Platform/Process/Process.h"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace NLS::Editor::Assets
{
struct EditorAssetRoot
{
    std::filesystem::path path;
    bool readOnly = false;
    std::filesystem::path mountPath;
    std::filesystem::path libraryPath;
};

inline std::string NormalizeEditorAssetPath(std::filesystem::path path)
{
    return path.lexically_normal().generic_string();
}

inline std::string NormalizeEditorProjectAssetPath(std::filesystem::path path)
{
    if (path.empty() || path.is_absolute())
        return {};

    const auto normalizedPath = path.lexically_normal();
    for (const auto& part : normalizedPath)
    {
        if (part == "..")
            return {};
    }

    const auto normalized = NormalizeEditorAssetPath(normalizedPath);
    if (normalized == "Assets" || normalized.starts_with("Assets/"))
        return normalized;
    return {};
}

inline std::optional<std::filesystem::path> ResolveEditorManifestDependencyPath(
    const std::filesystem::path& projectRoot,
    const std::string& dependencyValue)
{
    auto normalized = std::filesystem::path(NormalizeEditorAssetPath(dependencyValue));
    if (normalized.empty() || normalized.is_absolute())
        return std::nullopt;

    for (const auto& part : normalized)
    {
        if (part == "..")
            return std::nullopt;
    }

    return (projectRoot / normalized).lexically_normal();
}

inline bool HasNullusProjectFile(const std::filesystem::path& projectRoot)
{
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(projectRoot, error), end; iterator != end; iterator.increment(error))
    {
        if (error)
            return false;

        if (iterator->is_regular_file(error) && iterator->path().extension() == ".nullus")
            return true;
        error.clear();
    }

    return false;
}

inline void AppendBuiltInShaderAssetRoot(
    std::vector<EditorAssetRoot>& roots,
    const std::filesystem::path& projectRoot)
{
    const auto normalizedProjectRoot = NLS::Core::Assets::NormalizeAssetPath(projectRoot);
    if (normalizedProjectRoot.empty() || normalizedProjectRoot == normalizedProjectRoot.root_path())
        return;

    std::vector<std::filesystem::path> candidateRoots;
    const auto installShaderRoot =
        (NLS::Platform::Process::ResolveInstallResourceRoots().engineAssetsRoot /
            "Shaders").lexically_normal();
    if (!installShaderRoot.empty())
        candidateRoots.push_back(installShaderRoot);
#if defined(NLS_ROOT_DIR)
    candidateRoots.push_back(
        (std::filesystem::path(NLS_ROOT_DIR) /
            "App" /
            "Assets" /
            "Engine" /
            "Shaders").lexically_normal());
#endif

    std::error_code error;
    for (const auto& builtInShaderRoot : candidateRoots)
    {
        if (!std::filesystem::is_directory(builtInShaderRoot, error))
        {
            error.clear();
            continue;
        }

        roots.push_back({
            builtInShaderRoot,
            true,
            std::filesystem::path("Assets") / "Engine" / "Shaders",
            normalizedProjectRoot / "Library"
        });
        return;
    }
}

inline std::vector<EditorAssetRoot> MakeEditorAssetRoots(
    const std::vector<std::filesystem::path>& roots,
    const bool readOnly = false)
{
    std::vector<EditorAssetRoot> result;
    result.reserve(roots.size());
    for (const auto& root : roots)
    {
        if (root.empty())
            continue;

        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(root);
        if (normalized.empty() || normalized == normalized.root_path())
            continue;

        std::error_code error;
        const auto assetsRoot = normalized / "Assets";
        const bool looksLikeProjectRoot =
            normalized.filename() != "Assets" &&
            std::filesystem::exists(assetsRoot, error) &&
            std::filesystem::is_directory(assetsRoot, error);
        if (looksLikeProjectRoot)
        {
            result.push_back({assetsRoot.lexically_normal(), readOnly, "Assets", normalized / "Library"});
            AppendBuiltInShaderAssetRoot(result, normalized);
            continue;
        }

        result.push_back({normalized, readOnly, {}, {}});
    }
    return result;
}

inline std::vector<EditorAssetRoot> MakeProjectEditorAssetRoots(const std::filesystem::path& projectRoot)
{
    const auto normalized = NLS::Core::Assets::NormalizeAssetPath(projectRoot);
    if (normalized.empty() || normalized == normalized.root_path())
        return {};

    std::vector<EditorAssetRoot> roots {{
        normalized / "Assets",
        false,
        "Assets",
        normalized / "Library"
    }};

    AppendBuiltInShaderAssetRoot(roots, normalized);

    return roots;
}

inline std::filesystem::path GetEditorAssetRootLibraryPath(const EditorAssetRoot& root)
{
    return root.libraryPath.empty() ? root.path / "Library" : root.libraryPath;
}

inline bool IsPathInsideEditorAssetRoot(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root)
{
    if (candidate == root)
        return true;

    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative.is_absolute())
        return false;

    for (const auto& part : relative)
    {
        if (part == "..")
            return false;
    }
    return true;
}

inline std::optional<std::filesystem::path> TryWeaklyCanonicalEditorPath(const std::filesystem::path& path)
{
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (!error)
        return canonical.lexically_normal();
    return std::nullopt;
}

inline size_t EditorAssetRelativeDepth(const std::filesystem::path& relative)
{
    if (relative.empty() || relative == ".")
        return 0u;

    size_t depth = 0u;
    for (const auto& part : relative)
    {
        if (part != ".")
            ++depth;
    }
    return depth;
}

inline bool IsPhysicalPathInsideEditorAssetRoot(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root)
{
    const auto canonicalCandidate = TryWeaklyCanonicalEditorPath(candidate);
    const auto canonicalRoot = TryWeaklyCanonicalEditorPath(root);
    return canonicalCandidate.has_value() &&
        canonicalRoot.has_value() &&
        IsPathInsideEditorAssetRoot(*canonicalCandidate, *canonicalRoot);
}

inline bool IsPhysicalRegularFileInsideEditorAssetRoot(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(candidate, error);
    if (error ||
        !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
    {
        return false;
    }

    return IsPathInsideEditorAssetRoot(candidate, root) &&
        IsPhysicalPathInsideEditorAssetRoot(candidate, root);
}

inline std::optional<size_t> ExistingAncestorDepthInsideRoot(
    std::filesystem::path candidate,
    const std::filesystem::path& root)
{
    if (!IsPathInsideEditorAssetRoot(candidate, root))
        return std::nullopt;

    candidate = candidate.parent_path();
    std::error_code error;
    while (!candidate.empty() && IsPathInsideEditorAssetRoot(candidate, root))
    {
        const auto exists = std::filesystem::exists(candidate, error);
        if (error)
            return std::nullopt;

        if (exists)
        {
            if (!IsPhysicalPathInsideEditorAssetRoot(candidate, root))
                return std::nullopt;

            const auto relative = candidate.lexically_relative(root);
            return EditorAssetRelativeDepth(relative);
        }
        error.clear();

        if (candidate == root)
            break;

        const auto parent = candidate.parent_path();
        if (parent == candidate)
            break;
        candidate = parent;
    }
    return std::nullopt;
}

inline size_t EditorAssetRootSpecificity(const std::filesystem::path& root)
{
    return EditorAssetRelativeDepth(root.lexically_relative(root.root_path()));
}

inline const EditorAssetRoot* FindEditorAssetRootForAbsolutePath(
    const std::vector<EditorAssetRoot>& roots,
    const std::filesystem::path& absolutePath)
{
    const auto candidate = NLS::Core::Assets::NormalizeAssetPath(absolutePath);
    const EditorAssetRoot* bestRoot = nullptr;
    size_t bestSpecificity = 0u;

    for (const auto& root : roots)
    {
        if (!IsPathInsideEditorAssetRoot(candidate, root.path))
            continue;

        const auto specificity = EditorAssetRootSpecificity(root.path);
        if (!bestRoot || specificity > bestSpecificity)
        {
            bestRoot = &root;
            bestSpecificity = specificity;
        }
    }
    return bestRoot;
}

inline std::optional<std::filesystem::path> CandidateForEditorAssetRoot(
    const EditorAssetRoot& root,
    const std::filesystem::path& normalizedAssetPath)
{
    if (!root.mountPath.empty())
    {
        const auto mount = std::filesystem::path(NormalizeEditorAssetPath(root.mountPath));
        const auto relative = normalizedAssetPath.lexically_relative(mount);
        if (relative.empty() || relative.is_absolute())
            return std::nullopt;

        for (const auto& part : relative)
        {
            if (part == "..")
                return std::nullopt;
        }

        return NLS::Core::Assets::NormalizeAssetPath(root.path / relative);
    }

    return NLS::Core::Assets::NormalizeAssetPath(root.path / normalizedAssetPath);
}

inline std::filesystem::path ResolveEditorAssetPath(
    const std::vector<EditorAssetRoot>& roots,
    const std::string& assetPath)
{
    if (roots.empty())
        return {};

    const auto normalized = std::filesystem::path(NormalizeEditorAssetPath(assetPath));
    if (normalized.empty())
        return {};

    if (normalized == "." || normalized == "..")
        return {};

    if (normalized.is_absolute())
    {
        const auto candidate = NLS::Core::Assets::NormalizeAssetPath(normalized);
        const auto* root = FindEditorAssetRootForAbsolutePath(roots, candidate);
        if (!root || !IsPhysicalPathInsideEditorAssetRoot(candidate, root->path))
            return {};
        return candidate;
    }

    const EditorAssetRoot* bestRoot = nullptr;
    std::filesystem::path bestCandidate;
    size_t bestDepth = 0u;

    for (const auto& root : roots)
    {
        const auto candidate = CandidateForEditorAssetRoot(root, normalized);
        if (!candidate.has_value())
            continue;

        const auto* owner = FindEditorAssetRootForAbsolutePath(roots, *candidate);
        if (owner != &root)
            continue;

        if (!IsPathInsideEditorAssetRoot(*candidate, root.path))
            continue;

        std::error_code error;
        const auto exists = std::filesystem::exists(*candidate, error);
        if (error)
            continue;

        if (exists)
        {
            if (!IsPhysicalPathInsideEditorAssetRoot(*candidate, root.path))
                continue;
            return *candidate;
        }

        auto depth = ExistingAncestorDepthInsideRoot(*candidate, root.path);
        if (!depth.has_value())
            continue;

        if (!bestRoot || *depth > bestDepth)
        {
            bestRoot = &root;
            bestCandidate = *candidate;
            bestDepth = *depth;
        }
    }

    return bestRoot ? bestCandidate : std::filesystem::path {};
}

inline std::filesystem::path ResolveEditorAssetPath(
    const std::vector<std::filesystem::path>& roots,
    const std::string& assetPath)
{
    return ResolveEditorAssetPath(MakeEditorAssetRoots(roots), assetPath);
}

inline std::string ToEditorAssetPath(
    const std::vector<EditorAssetRoot>& roots,
    const std::filesystem::path& absolutePath)
{
    const auto normalized = NLS::Core::Assets::NormalizeAssetPath(absolutePath);
    const auto* root = FindEditorAssetRootForAbsolutePath(roots, normalized);
    if (!root)
        return NormalizeEditorAssetPath(normalized);

    const auto relative = normalized.lexically_relative(root->path);
    if (relative.empty())
        return {};

    if (!root->mountPath.empty())
        return NormalizeEditorAssetPath(root->mountPath / relative);
    return NormalizeEditorAssetPath(relative);
}

inline std::string ToEditorAssetPath(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path& absolutePath)
{
    return ToEditorAssetPath(MakeEditorAssetRoots(roots), absolutePath);
}

inline bool IsEditorAssetPathWritable(
    const std::vector<EditorAssetRoot>& roots,
    const std::filesystem::path& absolutePath)
{
    const auto* root = FindEditorAssetRootForAbsolutePath(roots, absolutePath);
    return root && !root->readOnly && IsPhysicalPathInsideEditorAssetRoot(absolutePath, root->path);
}
}
