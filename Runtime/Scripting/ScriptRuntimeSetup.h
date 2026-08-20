#pragma once

#include "ScriptRuntime.h"

#include <filesystem>
#include <cstdint>
#include <string>

namespace NLS::Scripting
{
struct NLS_SCRIPTING_API ScriptRuntimeSetupOptions
{
    // The project directory is used as the first search root.  The helper
    // walks its parents so a checkout layout such as TestProject/Assets can
    // still resolve the repository-local Library and Tools folders.
    std::filesystem::path projectRoot;
    std::filesystem::path scriptApiDirectory;
    std::filesystem::path dotnetRoot;
    std::filesystem::path managedRuntimeConfig;
    std::filesystem::path managedAssembly;
    bool enableCSharp = true;
    bool enableLua = true;
    bool enableLuaPanda = false;
    std::string luaPandaHost = "127.0.0.1";
    uint16_t luaPandaPort = 8818;
};

struct NLS_SCRIPTING_API ScriptRuntimeSetupResult
{
    ScriptStatus status;
    bool schemaLoaded = false;
    bool csharpRegistered = false;
    bool luaRegistered = false;
    // LuaPanda is an optional Editor-only capability.  A build without the
    // debugger (for example a Release Editor) must still initialize the Lua
    // backend when the project setting was enabled in another configuration.
    bool luaPandaEnabled = false;
    std::filesystem::path scriptApiDirectory;
    std::filesystem::path dotnetRoot;
    std::filesystem::path managedRuntimeConfig;
    std::filesystem::path managedAssembly;
};

// Resolve repository-local artifacts and initialize the caller-owned runtime.
// No environment PATH mutation or network access is performed here.
NLS_SCRIPTING_API ScriptRuntimeSetupResult InitializeScriptRuntime(
    ScriptRuntime& runtime,
    const ScriptRuntimeSetupOptions& options);
}
