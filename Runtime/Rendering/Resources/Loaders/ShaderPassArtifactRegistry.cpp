#include "Rendering/Resources/Loaders/ShaderPassArtifactRegistry.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace NLS::Render::Resources::Loaders
{
namespace
{
std::string NormalizeShaderSourcePath(const std::string_view path)
{
    if (path.empty())
        return {};
    return std::filesystem::path(path).lexically_normal().generic_string();
}

std::mutex g_mutex;
std::unordered_map<std::string, std::vector<std::string>> g_artifacts;
} // namespace

void RegisterShaderPassArtifacts(
    const std::string_view shaderSourcePath,
    std::vector<std::string> artifactPaths)
{
    const auto key = NormalizeShaderSourcePath(shaderSourcePath);
    if (key.empty() || artifactPaths.empty())
        return;

    std::lock_guard lock(g_mutex);
    auto& stored = g_artifacts[key];
    stored.insert(
        stored.end(),
        std::make_move_iterator(artifactPaths.begin()),
        std::make_move_iterator(artifactPaths.end()));
    std::sort(stored.begin(), stored.end());
    stored.erase(std::unique(stored.begin(), stored.end()), stored.end());
}

void ClearShaderPassArtifactRegistry()
{
    std::lock_guard lock(g_mutex);
    g_artifacts.clear();
}

std::vector<std::string> FindShaderPassArtifacts(const std::string_view shaderSourcePath)
{
    const auto key = NormalizeShaderSourcePath(shaderSourcePath);
    if (key.empty())
        return {};

    std::lock_guard lock(g_mutex);
    const auto found = g_artifacts.find(key);
    return found == g_artifacts.end() ? std::vector<std::string> {} : found->second;
}
} // namespace NLS::Render::Resources::Loaders
