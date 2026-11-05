#pragma once
#include "CoreDef.h"
#include "Guid.h"
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Core::Assets
{
// Unicode 15.1 NFC, independent of host filesystem normalization/casing.
NLS_CORE_API std::string NormalizePluginAssetPath(std::string_view path);
NLS_CORE_API std::string PluginAssetCollisionKey(std::string_view path);
NLS_CORE_API Guid PluginAssetGuid(std::string_view pluginId, std::string_view mount, std::string_view path);
struct PluginContentRoot
{
    std::filesystem::path physicalPath, logicalPath, libraryPath;
    std::string identityNamespace;
    bool editorOnly = false;
    std::vector<std::filesystem::path> importInputs;
    // A copied resource resolution may outlive the project session. Keep the
    // immutable package content read lease until its last consumer releases it.
    std::shared_ptr<const void> contentLease;
};
struct PluginContentRoots { std::vector<PluginContentRoot> roots; };
}
