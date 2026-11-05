#pragma once
#include "Manifest.h"
#include <functional>

namespace NLS::Plugins
{
class PluginManager;
NLS_PLUGINS_API std::string HashPluginFile(const std::filesystem::path& path);
NLS_PLUGINS_API std::string HashPluginBuildInputs(const std::filesystem::path& root);
// Consent covers every package file, including indirectly executed scripts and
// tools with arbitrary extensions. Generated outputs must live outside root.
NLS_PLUGINS_API std::string HashPluginBuildApprovalInputs(const std::filesystem::path& root);
// installRoot must be a fresh build staging directory, not a live install.
NLS_PLUGINS_API bool StageRuntimePlugins(const std::filesystem::path& project,
    const std::filesystem::path& installRoot, const std::string& platform,
    const std::string& architecture, std::string* error = nullptr,const std::string& configuration = {});
NLS_PLUGINS_API int RunRuntimePluginSelfTest(const std::filesystem::path& installRoot,std::function<void(PluginManager&)> registerSources = {});
}
