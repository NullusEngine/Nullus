#pragma once

#include "Properties/PropertyDrawerRegistry.h"
#include "Serialization/SerializedProperty.h"
#include <UI/Controls/CollectionControl.h>

namespace NLS::Editor::Properties
{
// Clipboard snapshots own their bytes and persistent references, never targets
// or cursors. The single-value slot is shared with ordinary property menus.
struct CollectionClipboard
{
    std::optional<Serialization::SerializedValue> value;
    std::vector<Serialization::SerializedValue> elements;
};
CollectionClipboard& GetCollectionClipboard();
Inspector::PropertyNode MakeCollectionElementNode(const Inspector::PropertyNode& parent,
                                                  const Serialization::SerializedProperty& element, size_t index);
UI::Controls::CollectionCommandResult ExecuteCollectionCommand(Serialization::SerializedObject& object,
                                                               const Inspector::PropertyNode& property, const UI::Controls::CollectionCommand& command,
                                                               CollectionClipboard& clipboard);
bool DrawCollectionProperty(PropertyDrawerContext& context);
} // namespace NLS::Editor::Properties
