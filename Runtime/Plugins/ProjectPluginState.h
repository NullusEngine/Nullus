#pragma once
#include "PluginsDef.h"
#include <filesystem>
#include <string>

namespace NLS::Plugins
{
// All callers hold Packages/.write.lock. Recovery is shared by the Editor,
// runtime metadata reader, installer and build tools; no second journal format.
NLS_PLUGINS_API bool IsProjectPluginStateFile(const std::string& name);
NLS_PLUGINS_API void RecoverProjectPluginStateLocked(const std::filesystem::path& project);
NLS_PLUGINS_API std::string ProjectPluginStateFingerprintLocked(const std::filesystem::path& project);
}
