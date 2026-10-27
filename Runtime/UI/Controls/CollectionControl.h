#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "UI/Controls/Control.h"

namespace NLS::UI::Controls
{
enum class CollectionAction
{
    Add,
    Remove,
    Duplicate,
    Move,
    Resize,
    Copy,
    Paste,
    CopyCollection,
    PasteCollection,
    Clear,
    ExpandAll,
    CollapseAll
};

struct CollectionCommand
{
    CollectionAction action = CollectionAction::Add;
    std::vector<size_t> indices;
    // A gap in the original ordering, in [0, count]. Selected elements are
    // removed before inserting at that gap. This also supports block moves.
    size_t insertion = 0;
    size_t size = 0;
};

struct CollectionCommandResult
{
    bool changed = false;
    size_t count = 0;
    uint64_t revision = 0;
    std::vector<size_t> selection;
    std::string error;
};

struct CollectionState
{
    bool expanded = false;
    bool focused = false;
    std::vector<size_t> selection;
    size_t anchor = 0, current = 0;
    bool pressedHandle = false, dragging = false;
    ImVec2 pressPosition{};
    std::vector<size_t> dragged;
    size_t insertion = 0;
    uint64_t revision = 0, dataRevision = 0;
    size_t count = 0;
    float width = 0, scale = 0;
    std::vector<float> heights, offsets, animatedOffsets;
    int requestedSize = 0;
    bool editingSize = false;
    std::string error;
};

struct CollectionOptions
{
    std::string label;
    std::string tooltip;
    size_t count = 0;
    size_t primaryCount = 0;
    uint64_t revision = 0;
    uint64_t dataRevision = 0;
    bool mixedSize = false;
    bool readOnly = false;
    bool fixedSize = false;
    bool reorderable = true;
    bool multiSelect = true;
    bool overMultiEditLimit = false;
    size_t multiEditLimit = 0;
    size_t maximumSize = static_cast<size_t>((std::numeric_limits<int>::max)());
    float scale = 1;
    float width = 0;
    float elementLabelWidth = 90;
};

struct CollectionCallbacks
{
    // Row callbacks draw values only. Composite rows draw their own foldout
    // label; scalar rows receive a standard "Element N" label from the host.
    std::function<bool(size_t)> composite;
    std::function<ControlResult(size_t, float)> drawElement;
    std::function<CollectionCommandResult(const CollectionCommand&)> execute;
    // Optional domain drop adapter. It is called on the list header/body item
    // and owns payload validation. It never survives the current draw.
    std::function<CollectionCommandResult()> drop;
    std::function<bool(CollectionAction)> available;
    std::function<void()> drawMenu;
};

struct CollectionPalette
{
    ImU32 background, header, footer, border, selection, selectionInactive, handle, insertion;
};

struct CollectionGeometry
{
    float header = 18, headerGap = 3, row = 21, footer = 20;
    float handle = 20, padding = 6, sizeWidth = 48, top = 4, bottom = 4;
};

NLS_UI_API CollectionPalette GetCollectionPalette(bool dark);
NLS_UI_API CollectionGeometry GetCollectionGeometry(float scale, float fontHeight);
NLS_UI_API void SelectCollectionRow(CollectionState& state, size_t index, size_t count, bool toggle, bool range);
NLS_UI_API std::vector<size_t> CollectionMoveOrder(size_t count, std::span<const size_t> selected, size_t insertion);
NLS_UI_API void SynchronizeCollection(CollectionState& state, const CollectionOptions& options);
NLS_UI_API ControlResult Collection(const char* id, CollectionState& state, const CollectionOptions& options,
                                    const CollectionCallbacks& callbacks);
} // namespace NLS::UI::Controls
