#include "Serialization/EditorSerializationManagedBridge.h"

#include <cstdint>

#include <Scripting/CoreClrScriptBackend.h>

namespace NLS::Editor::Serialization
{
namespace
{
constexpr const char* kEditorSerializationHostType = "Nullus.Editor.EditorSerializationHost";
constexpr const char* kEditorSerializationHostQualifiedType = "Nullus.Editor.EditorSerializationHost, Nullus.Editor";
constexpr const char* kInstallMethod = "Install";
constexpr const char* kUninstallMethod = "Uninstall";
constexpr const char* kManagedEditorAssemblyName = "Nullus.Editor.dll";
constexpr const char* kManagedEditorAssemblyStem = "Nullus.Editor";

// `[UnmanagedCallersOnly] static int Install(nint)` / `static void Uninstall()`.
using InstallTableFn = int32_t (*)(const EditorSerializationApiTable*);
using UninstallTableFn = void (*)();
} // namespace

std::filesystem::path ResolveManagedEditorAssembly(const std::filesystem::path& scriptingAssembly)
{
    if (scriptingAssembly.empty())
        return {};
    const auto directory = scriptingAssembly.parent_path();
    if (directory.empty())
        return {};
    // The CMake managed rule writes Nullus.Editor.dll beside the project
    // scripting assembly; a miss just means this build has no managed editor.
    std::error_code error;
    const auto candidate = directory / kManagedEditorAssemblyName;
    if (std::filesystem::is_regular_file(candidate, error))
        return candidate;
    return {};
}

EditorSerializationBridgeResult InstallManagedSerializationBridge(
    const NLS::Scripting::CoreClrHost& host,
    const std::filesystem::path& managedEditorAssembly)
{
    if (!host.IsLoaded())
        return {false, "CoreCLR host is not loaded."};
    if (managedEditorAssembly.empty()
        || managedEditorAssembly.stem().string() != kManagedEditorAssemblyStem)
    {
        return {false, "Managed editor assembly 'Nullus.Editor.dll' is not available."};
    }

    void* entryPoint = nullptr;
    auto resolved = host.GetUnmanagedFunctionPointer(
        managedEditorAssembly,
        kEditorSerializationHostType,
        kInstallMethod,
        entryPoint);
    if (!resolved.Succeeded())
    {
        // hostfxr accepts both the assembly-local and the assembly-qualified
        // type name; the qualified form is the reliable one when the assembly
        // is not the runtime's application assembly.
        resolved = host.GetUnmanagedFunctionPointer(
            managedEditorAssembly,
            kEditorSerializationHostQualifiedType,
            kInstallMethod,
            entryPoint);
    }
    if (!resolved.Succeeded() || entryPoint == nullptr)
    {
        return {false, resolved.Succeeded()
            ? std::string("Managed editor serialization entry point resolved to null.")
            : resolved.message};
    }

    // The table is a static owned by this module, so the managed side may keep
    // the pointer for the rest of the domain. The managed entry point never
    // throws: it returns 1 for a null table and 2 for an incompatible header.
    const auto result = reinterpret_cast<InstallTableFn>(entryPoint)(&GetEditorSerializationApiTable());
    if (result == 0)
        return {true, {}};

    return {false, result == 1
        ? std::string("Managed editor serialization bridge rejected a null ABI table.")
        : std::string("Managed editor serialization bridge rejected the native ABI header.")};
}

void UninstallManagedSerializationBridge(
    const NLS::Scripting::CoreClrHost& host,
    const std::filesystem::path& managedEditorAssembly)
{
    if (!host.IsLoaded() || managedEditorAssembly.empty())
        return;
    void* entryPoint = nullptr;
    auto resolved = host.GetUnmanagedFunctionPointer(
        managedEditorAssembly,
        kEditorSerializationHostType,
        kUninstallMethod,
        entryPoint);
    if (!resolved.Succeeded())
    {
        resolved = host.GetUnmanagedFunctionPointer(
            managedEditorAssembly,
            kEditorSerializationHostQualifiedType,
            kUninstallMethod,
            entryPoint);
    }
    if (!resolved.Succeeded() || entryPoint == nullptr)
    {
        return;
    }
    reinterpret_cast<UninstallTableFn>(entryPoint)();
}
} // namespace NLS::Editor::Serialization
