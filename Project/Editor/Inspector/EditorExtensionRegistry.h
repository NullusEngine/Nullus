#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "Inspector/InspectorRegistry.h"
#include "Reflection/TypeKey.h"

namespace NLS::Editor::Inspector
{
// MetaParser emits these descriptors from NLS_EDITOR_CUSTOM_EDITOR metadata.
// They are intentionally plain module data: no global constructors, raw
// object pointers, or link-order-dependent registration is involved.
enum class EditorExtensionKind : uint8_t
{
    ComponentEditor
};

struct EditorExtensionDescriptor
{
    EditorExtensionKind kind = EditorExtensionKind::ComponentEditor;
    meta::TypeKey inspectedTypeKey {};
    const char* inspectedTypeName = nullptr;
    meta::TypeKey editorTypeKey {};
    const char* editorTypeName = nullptr;
    const char* identifier = nullptr;
    bool editorForChildClasses = false;
    bool isFallback = false;
    bool supportsMultiObject = false;
    int32_t priority = 0;
    std::unique_ptr<ComponentEditor> (*create)() = nullptr;
};

// May be called at editor startup, after a plugin load, and after a managed
// domain reload. Per-registry descriptor fingerprints prevent a repeated scan
// from registering the same generated declaration twice; a changed or
// conflicting declaration still reaches InspectorRegistry and fails loudly.
void RegisterEditorExtensionDescriptors(
    InspectorRegistry& registry,
    std::span<const EditorExtensionDescriptor> descriptors);
} // namespace NLS::Editor::Inspector
