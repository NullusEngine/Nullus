#pragma once

// Editor extension declarations are intentionally metadata only. MetaParser
// reads these invocations from the source header and emits the module's
// descriptor table; the C++ compiler sees no static registration side effect.
//
// The target and editor type names are resolved by the generated table through
// stable reflection TypeKeys after the reflection database is ready.
#define NLS_EDITOR_CUSTOM_EDITOR(target_type, editor_type)

// Arguments after the target/editor pair are: editorForChildClasses,
// isFallback, supportsMultiObject, priority.
#define NLS_EDITOR_CUSTOM_EDITOR_WITH_OPTIONS( \
    target_type, editor_type, child_classes, fallback_editor, multi_object, priority)
