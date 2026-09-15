#include "Scripting/EditorScriptBridge.h"

#include <cstdint>
#include <string>

#include <Debug/Logger.h>
#include <Scripting/CoreClrScriptBackend.h>

#include "Scripting/EditorScriptHost.h"

namespace NLS::Editor::Scripting
{
namespace
{
// The stable entry point lives in the managed editor assembly, which the host
// resolves once per process and never unloads.  That host loads the project
// editor scripts into a collectible load context, so calling Install again
// replaces the previously loaded scripts.
constexpr const char* kEditorHostType = "Nullus.Editor.EditorScriptingHost";
constexpr const char* kEditorHostQualifiedType = "Nullus.Editor.EditorScriptingHost, Nullus.Editor";
constexpr const char* kInstallMethod = "Install";
constexpr const char* kUninstallMethod = "Uninstall";
constexpr const char* kEditorScriptsAssemblyName = "GameScriptsEditor.dll";
constexpr uint32_t kExpectedAbiVersion = 1u;

// `[UnmanagedCallersOnly] static nint Install(nint hostApi, nint editorScriptsPath)`.
// The path is UTF-8 and borrowed for the duration of the call.
using InstallFn = ManagedEditorScriptApiTable* (*)(const EditorScriptHostApiTable*, const char*);
using UninstallFn = void (*)();

} // namespace

std::filesystem::path ResolveEditorScriptsAssembly(const std::filesystem::path& managedAssemblyDirectory)
{
    if (managedAssemblyDirectory.empty())
        return {};

    std::error_code error;
    const auto candidate = managedAssemblyDirectory / kEditorScriptsAssemblyName;
    if (std::filesystem::is_regular_file(candidate, error))
        return candidate;
    return {};
}

EditorScriptBridgeResult InstallEditorScriptBridge(
    const NLS::Scripting::CoreClrHost& host,
    const std::filesystem::path& managedEditorAssembly,
    const std::filesystem::path& editorScriptsAssembly)
{
    if (!host.IsLoaded())
        return {false, "CoreCLR host is not loaded."};
    if (managedEditorAssembly.empty() || managedEditorAssembly.stem().string() != "Nullus.Editor")
        return {false, "Managed editor assembly 'Nullus.Editor.dll' is not available."};

    void* entryPoint = nullptr;
    auto resolved = host.GetUnmanagedFunctionPointer(
        managedEditorAssembly,
        kEditorHostType,
        kInstallMethod,
        entryPoint);
    if (!resolved.Succeeded())
    {
        // hostfxr accepts both the assembly-local and the assembly-qualified
        // type name; the qualified form is the reliable one when the assembly
        // is not the runtime's application assembly.
        resolved = host.GetUnmanagedFunctionPointer(
            managedEditorAssembly,
            kEditorHostQualifiedType,
            kInstallMethod,
            entryPoint);
    }
    if (!resolved.Succeeded() || entryPoint == nullptr)
    {
        return {false, resolved.Succeeded()
            ? std::string("The managed editor script host resolved to null.")
            : resolved.message};
    }

    auto& scriptHost = EditorScriptHost::Get();
    scriptHost.ResetManagedApi();

    // A missing editor-scripts assembly is a valid configuration: the host then
    // installs the table with no editor scripts and the editor keeps running
    // with the native-only surface.
    const auto pathStorage = editorScriptsAssembly.generic_string();
    const auto* path = pathStorage.empty() ? nullptr : pathStorage.c_str();
    const auto* managed = reinterpret_cast<InstallFn>(entryPoint)(&GetEditorScriptHostApiTable(), path);
    if (managed == nullptr)
        return {false, "The managed editor script host returned no API table."};
    if (managed->header.abiVersion != kExpectedAbiVersion)
        return {false, "The managed editor script host reported an incompatible ABI version."};

    scriptHost.SetManagedApi(managed);
    scriptHost.RescanManagedRegistrations();
    return {true, {}};
}

void UninstallEditorScriptBridge(
    const NLS::Scripting::CoreClrHost& host,
    const std::filesystem::path& managedEditorAssembly,
    const std::filesystem::path& editorScriptsAssembly)
{
    (void)editorScriptsAssembly;

    if (host.IsLoaded() && !managedEditorAssembly.empty())
    {
        void* entryPoint = nullptr;
        auto resolved = host.GetUnmanagedFunctionPointer(
            managedEditorAssembly,
            kEditorHostType,
            kUninstallMethod,
            entryPoint);
        if (!resolved.Succeeded())
        {
            resolved = host.GetUnmanagedFunctionPointer(
                managedEditorAssembly,
                kEditorHostQualifiedType,
                kUninstallMethod,
                entryPoint);
        }
        if (resolved.Succeeded() && entryPoint != nullptr)
            reinterpret_cast<UninstallFn>(entryPoint)();
    }

    auto& scriptHost = EditorScriptHost::Get();
    scriptHost.ResetManagedApi();
    scriptHost.ApplyComponentEditorRegistrations();
}
} // namespace NLS::Editor::Scripting
