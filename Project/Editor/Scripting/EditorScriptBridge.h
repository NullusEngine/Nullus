#pragma once

#include <filesystem>
#include <string>

namespace NLS::Scripting
{
class CoreClrHost;
}

namespace NLS::Editor::Scripting
{
struct EditorScriptBridgeResult
{
    bool installed = false;
    std::string message;
};

// Resolves the project editor-scripts assembly beside the managed editor
// assembly.  An empty path means this project has no Editor directory, which is
// a normal configuration rather than an error.
[[nodiscard]] std::filesystem::path ResolveEditorScriptsAssembly(
    const std::filesystem::path& managedAssemblyDirectory);

// Resolves the stable editor-host entry point in the managed editor assembly,
// hands it the native host table plus the path of the freshly built project
// editor-scripts assembly, stores the managed table it returns, and runs the
// descriptor scan.  The managed host loads the editor scripts into a
// collectible load context, so calling this again after a project rebuild
// replaces the previously loaded editor scripts.
//
// Failure is never fatal: the editor keeps running with the native-only
// surface and the reason is reported once.
EditorScriptBridgeResult InstallEditorScriptBridge(
    const NLS::Scripting::CoreClrHost& host,
    const std::filesystem::path& managedEditorAssembly,
    const std::filesystem::path& editorScriptsAssembly);

// Clears the managed table and any scripted registrations so a reloaded domain
// never observes a pointer into a released table.
void UninstallEditorScriptBridge(
    const NLS::Scripting::CoreClrHost& host,
    const std::filesystem::path& managedEditorAssembly,
    const std::filesystem::path& editorScriptsAssembly);
} // namespace NLS::Editor::Scripting
