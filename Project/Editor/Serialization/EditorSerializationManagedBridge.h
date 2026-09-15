#pragma once

#include <filesystem>
#include <string>

#include "Serialization/EditorSerializationAbi.h"

namespace NLS::Scripting
{
class CoreClrHost;
}

namespace NLS::Editor::Serialization
{
struct EditorSerializationBridgeResult
{
    bool installed = false;
    std::string message;
};

// Publishes the native SerializedObject/SerializedProperty ABI table to the
// managed editor assembly (Nullus.Editor) through the already-loaded CoreCLR
// host. The managed bridge keeps the pointer for the rest of the domain, so
// every later `new SerializedObject(target)` resolves its handle against it.
//
// Failure is not fatal for the editor: a host without the managed editor
// assembly simply keeps running with the native-only surface. The result
// carries the reason so the caller can log it once.
EditorSerializationBridgeResult InstallManagedSerializationBridge(
    const NLS::Scripting::CoreClrHost& host,
    const std::filesystem::path& managedEditorAssembly);

// Clears the managed table before a domain or host teardown so a reloaded
// domain never observes a pointer into a released native table.
void UninstallManagedSerializationBridge(
    const NLS::Scripting::CoreClrHost& host,
    const std::filesystem::path& managedEditorAssembly);

// Directory-relative lookup used by hosts that only know the scripting
// assembly path; returns an empty path when the editor assembly is absent.
std::filesystem::path ResolveManagedEditorAssembly(const std::filesystem::path& scriptingAssembly);
} // namespace NLS::Editor::Serialization
