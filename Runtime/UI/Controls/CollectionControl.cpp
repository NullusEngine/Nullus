#include "UI/Controls/CollectionControl.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <numeric>
#include <string>
#include <imgui_internal.h>

namespace NLS::UI::Controls
{
CollectionPalette GetCollectionPalette(bool dark)
{
    if (dark)
        return {IM_COL32(45, 45, 45, 255), IM_COL32(60, 60, 60, 255), IM_COL32(55, 55, 55, 255), IM_COL32(26, 26, 26, 255), IM_COL32(44, 93, 135, 255), IM_COL32(65, 65, 65, 255), IM_COL32(145, 145, 145, 255), IM_COL32(65, 145, 230, 255)};
    return {IM_COL32(223, 223, 223, 255), IM_COL32(205, 205, 205, 255), IM_COL32(205, 205, 205, 255), IM_COL32(153, 153, 153, 255), IM_COL32(62, 125, 231, 255), IM_COL32(174, 174, 174, 255), IM_COL32(95, 95, 95, 255), IM_COL32(35, 100, 210, 255)};
}

CollectionGeometry GetCollectionGeometry(float scale, float fontHeight)
{
    scale = (std::max)(scale, .1f);
    CollectionGeometry g;
    g.header = (std::max)(18 * scale, fontHeight + 2 * scale);
    g.headerGap = 3 * scale;
    g.row = (std::max)(21 * scale, g.header + 3 * scale);
    g.footer = 20 * scale;
    g.handle = 20 * scale;
    g.padding = 6 * scale;
    g.sizeWidth = 48 * scale;
    g.top = 4 * scale;
    g.bottom = 4 * scale;
    return g;
}

void SelectCollectionRow(CollectionState& state, size_t index, size_t count, bool toggle, bool range)
{
    if (index >= count)
        return;
    if (range && !state.selection.empty())
    {
        if (!toggle)
            state.selection.clear();
        const auto first = (std::min)(state.anchor, index), last = (std::max)(state.anchor, index);
        for (size_t i = first; i <= last; ++i)
            state.selection.push_back(i);
    }
    else if (toggle)
    {
        const auto found = std::find(state.selection.begin(), state.selection.end(), index);
        if (found == state.selection.end())
            state.selection.push_back(index);
        else
            state.selection.erase(found);
        state.anchor = index;
    }
    else
    {
        state.selection = {index};
        state.anchor = index;
    }
    std::sort(state.selection.begin(), state.selection.end());
    state.selection.erase(std::unique(state.selection.begin(), state.selection.end()), state.selection.end());
    state.current = index;
    state.focused = true;
}

std::vector<size_t> CollectionMoveOrder(size_t count, std::span<const size_t> selected, size_t insertion)
{
    if (insertion > count)
        return {};
    std::vector<size_t> block(selected.begin(), selected.end());
    std::sort(block.begin(), block.end());
    if (block.empty() || block.back() >= count || std::adjacent_find(block.begin(), block.end()) != block.end())
        return {};
    std::vector<size_t> order;
    order.reserve(count);
    for (size_t i = 0; i < count; ++i)
        if (!std::binary_search(block.begin(), block.end(), i))
            order.push_back(i);
    const auto before = static_cast<size_t>(std::lower_bound(block.begin(), block.end(), insertion) - block.begin());
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(insertion - before), block.begin(), block.end());
    return order;
}

void SynchronizeCollection(CollectionState& state, const CollectionOptions& options)
{
    if ((state.dragging || state.pressedHandle || state.editingSize) && (state.revision != options.revision || state.count != options.count || state.dataRevision != options.dataRevision || options.readOnly || options.fixedSize || (!options.reorderable && (state.dragging || state.pressedHandle))))
    {
        state.dragging = state.pressedHandle = state.editingSize = false;
        state.dragged.clear();
    }
    std::erase_if(state.selection, [&](size_t i)
                  { return i >= options.count; });
    if (options.count)
    {
        state.current = (std::min)(state.current, options.count - 1);
        state.anchor = (std::min)(state.anchor, options.count - 1);
    }
    else
    {
        state.current = state.anchor = 0;
        state.selection.clear();
    }
    if (state.heights.size() != options.count || state.count != options.count || state.revision != options.revision || std::abs(state.width - options.width) > .5f || state.scale != options.scale)
    {
        state.heights.assign(options.count, 0);
        state.animatedOffsets.assign(options.count, std::numeric_limits<float>::quiet_NaN());
        state.offsets.clear();
    }
    state.dataRevision = options.dataRevision;
    state.revision = options.revision;
    state.count = options.count;
    state.width = options.width;
    state.scale = options.scale;
    if (!state.editingSize)
        state.requestedSize = static_cast<int>((std::min)(options.primaryCount, static_cast<size_t>(INT_MAX)));
}

namespace
{
bool ActionKey(const ImGuiIO& io)
{
    // ImGui normalizes physical Command to KeyCtrl on macOS.
    return io.KeyCtrl;
}
bool Contains(ImVec2 min, ImVec2 max, ImVec2 p)
{
    return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y;
}
void Handle(ImDrawList* draw, ImVec2 min, const CollectionGeometry& g, ImU32 color, float height)
{
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 2; ++x)
            draw->AddRectFilled({min.x + g.padding + x * 3 * g.padding / 6, min.y + height * .5f + (y - 1) * 3 * g.padding / 6},
                                {min.x + g.padding + x * 3 * g.padding / 6 + g.padding / 3, min.y + height * .5f + (y - 1) * 3 * g.padding / 6 + g.padding / 3},
                                color);
}
} // namespace

ControlResult Collection(const char* id, CollectionState& state, const CollectionOptions& input, const CollectionCallbacks& callbacks)
{
    ControlResult result;
    auto options = input;
    options.width = options.width > 0 ? options.width : ImGui::GetContentRegionAvail().x;
    options.width = (std::max)(options.width, 1.f);
    if (options.primaryCount == 0 && options.count > 0)
        options.primaryCount = options.count;
    const bool pendingGesture = state.dragging || state.pressedHandle || state.editingSize;
    SynchronizeCollection(state, options);
    result.canceled = pendingGesture && !state.dragging && !state.pressedHandle && !state.editingSize;
    const auto g = GetCollectionGeometry(options.scale, ImGui::GetFontSize());
    const bool dark = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).x < .5f;
    const auto colors = GetCollectionPalette(dark);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const auto controlId = ImGui::GetID(id);
    auto* window = ImGui::GetCurrentWindow();
    const auto focusKey = ImHashStr("CollectionKeyboardFocus", 0, window->ID);
    const auto focus = [&]
    {state.focused=true;window->StateStorage.SetInt(focusKey, static_cast<int>(controlId)); };
    state.focused = static_cast<ImGuiID>(window->StateStorage.GetInt(focusKey)) == controlId;
    ImGui::PushID(id);
    if (result.canceled && GImGui->ActiveId == ImGui::GetID("##Size"))
        ImGui::ClearActiveID();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2 * options.scale, (std::max)(0.f, (g.header - ImGui::GetFontSize()) * .5f)});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, options.scale);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, dark ? IM_COL32(38, 38, 38, 255) : IM_COL32(238, 238, 238, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, dark ? IM_COL32(48, 48, 48, 255) : IM_COL32(248, 248, 248, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, dark ? IM_COL32(32, 32, 32, 255) : IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_Button, colors.footer);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dark ? IM_COL32(70, 70, 70, 255) : IM_COL32(215, 215, 215, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, dark ? IM_COL32(80, 80, 80, 255) : IM_COL32(185, 185, 185, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, colors.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4 * options.scale, 2 * options.scale});
    ImGui::BeginGroup();
    bool structureChanged = false, layoutChanged = false;
    const auto execute = [&](CollectionCommand command)
    {
        if (!callbacks.execute)
            return false;
        focus();
        auto applied = callbacks.execute(command);
        state.error = applied.error;
        if (!applied.changed)
        {
            if (command.action == CollectionAction::ExpandAll || command.action == CollectionAction::CollapseAll)
                layoutChanged = true;
            return false;
        }
        if (!state.dragging && !state.pressedHandle && !state.editingSize)
            result.started = true;
        result.changed = result.completed = true;
        structureChanged = true;
        state.revision = applied.revision;
        state.count = applied.count;
        state.selection = std::move(applied.selection);
        if (!state.selection.empty())
            state.anchor = state.current = state.selection.front();
        state.dragging = state.pressedHandle = false;
        state.dragged.clear();
        return true;
    };
    const auto available = [&](CollectionAction action)
    { return !callbacks.available || callbacks.available(action); };

    const float countWidth = (std::min)(g.sizeWidth, options.width * .35f);
    const ImVec2 headerMax{origin.x + options.width, origin.y + g.header};
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##Foldout", {(std::max)(1.f, options.width - countWidth - g.padding), g.header});
    if (ImGui::IsItemClicked())
    {
        state.expanded = !state.expanded;
        focus();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        focus();
        ImGui::OpenPopup("##CollectionMenu");
    }
    const float arrowX = origin.x + g.padding * .5f, arrowY = origin.y + g.header * .5f;
    const auto text = ImGui::GetColorU32(ImGuiCol_Text);
    if (state.expanded)
        draw->AddTriangleFilled({arrowX, arrowY - 2 * options.scale}, {arrowX + 8 * options.scale, arrowY - 2 * options.scale}, {arrowX + 4 * options.scale, arrowY + 3 * options.scale}, text);
    else
        draw->AddTriangleFilled({arrowX + 2 * options.scale, arrowY - 4 * options.scale}, {arrowX + 2 * options.scale, arrowY + 4 * options.scale}, {arrowX + 7 * options.scale, arrowY}, text);
    draw->PushClipRect(origin, {headerMax.x - countWidth - g.padding, headerMax.y}, true);
    draw->AddText({origin.x + 15 * options.scale, origin.y + (g.header - ImGui::GetFontSize()) * .5f}, text, options.label.c_str());
    draw->PopClipRect();
    if (!options.tooltip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", options.tooltip.c_str());
    const auto drop = [&]
    {
        auto dropped = callbacks.drop();
        if (dropped.changed)
        {
            focus();
            result.started = result.changed = result.completed = true;
            structureChanged = true;
            state.selection = std::move(dropped.selection);
            state.revision = dropped.revision;
            state.count = dropped.count;
        }
        if (!dropped.error.empty())
            state.error = dropped.error;
    };
    if (callbacks.drop && !options.readOnly && !options.fixedSize && ImGui::BeginDragDropTarget())
    {
        drop();
        ImGui::EndDragDropTarget();
    }
    ImGui::SetCursorScreenPos({headerMax.x - countWidth, origin.y});
    ImGui::SetNextItemWidth(countWidth);
    ImGui::BeginDisabled(options.readOnly || options.fixedSize);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2 * options.scale, (std::max)(0.f, (g.header - ImGui::GetFontSize()) * .5f)});
    if (options.mixedSize && !state.editingSize)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    const bool submitted = ImGui::InputInt("##Size", &state.requestedSize, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
    bool sizeActive = ImGui::IsItemActive();
    const bool confirmSize = state.editingSize && (sizeActive || ImGui::IsItemDeactivated()) && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
    if (options.mixedSize && !state.editingSize)
        ImGui::PopStyleColor();
    if (options.mixedSize && !sizeActive)
    {
        const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        draw->AddRectFilled({a.x + options.scale, a.y + options.scale}, {b.x - options.scale, b.y - options.scale}, ImGui::GetColorU32(ImGuiCol_FrameBg));
        const ImVec2 center{(a.x + b.x) * .5f, (a.y + b.y) * .5f};
        draw->AddLine({center.x - 4 * options.scale, center.y}, {center.x + 4 * options.scale, center.y}, ImGui::GetColorU32(ImGuiCol_Text), options.scale);
    }
    if (ImGui::IsItemActivated())
    {
        state.editingSize = true;
        result.started = true;
        focus();
    }
    if (state.editingSize && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        state.editingSize = false;
        result.canceled = true;
        sizeActive = false;
        ImGui::ClearActiveID();
        state.requestedSize = static_cast<int>((std::min)(options.primaryCount, static_cast<size_t>(INT_MAX)));
    }
    else if (state.editingSize && (submitted || confirmSize || ImGui::IsItemDeactivated()))
    {
        if (submitted || confirmSize || ImGui::IsItemDeactivatedAfterEdit())
        {
            CollectionCommand command;
            command.action = CollectionAction::Resize;
            command.size = (std::min)(static_cast<size_t>((std::max)(0, state.requestedSize)), options.maximumSize);
            execute(std::move(command));
        }
        result.completed = state.error.empty();
        result.canceled |= !state.error.empty();
        state.editingSize = false;
        if (confirmSize)
        {
            ImGui::ClearActiveID();
            sizeActive = false;
        }
    }
    result.active |= sizeActive;
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    ImGui::SetCursorScreenPos({origin.x, headerMax.y + g.headerGap});
    const auto menu = [&](bool element)
    {
        const bool mutate = !options.readOnly && !options.fixedSize;
        if (element)
        {
            if (ImGui::MenuItem("Duplicate Array Element", nullptr, false, mutate && available(CollectionAction::Duplicate)))
                execute({CollectionAction::Duplicate, state.selection});
            if (ImGui::MenuItem("Delete Array Element", nullptr, false, mutate && available(CollectionAction::Remove)))
                execute({CollectionAction::Remove, state.selection});
            ImGui::Separator();
            if (ImGui::MenuItem("Copy", nullptr, false, available(CollectionAction::Copy)))
                execute({CollectionAction::Copy, state.selection});
            if (ImGui::MenuItem("Paste", nullptr, false, !options.readOnly && available(CollectionAction::Paste)))
                execute({CollectionAction::Paste, state.selection});
        }
        else
        {
            if (ImGui::MenuItem("Copy", nullptr, false, available(CollectionAction::CopyCollection)))
                execute({CollectionAction::CopyCollection});
            if (ImGui::MenuItem("Paste", nullptr, false, mutate && available(CollectionAction::PasteCollection)))
                execute({CollectionAction::PasteCollection});
            if (ImGui::MenuItem("Clear", nullptr, false, mutate && (options.primaryCount > 0 || options.mixedSize)))
                execute({CollectionAction::Clear});
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Expand All"))
            execute({CollectionAction::ExpandAll});
        if (ImGui::MenuItem("Collapse All"))
            execute({CollectionAction::CollapseAll});
        if (!element && callbacks.drawMenu)
        {
            ImGui::Separator();
            callbacks.drawMenu();
        }
    };
    if (ImGui::BeginPopup("##CollectionMenu"))
    {
        menu(false);
        ImGui::EndPopup();
    }

    if (state.expanded && !structureChanged)
    {
        const ImVec2 body = ImGui::GetCursorScreenPos();
        for (auto& height : state.heights)
            if (height <= 0)
                height = g.row;
        state.offsets.resize(options.count + 1);
        state.offsets[0] = 0;
        for (size_t i = 0; i < options.count; ++i)
            state.offsets[i + 1] = state.offsets[i] + state.heights[i];
        const float emptyHeight = options.overMultiEditLimit ? g.row * 2 : g.row;

        ImDrawListSplitter splitter;
        splitter.Split(draw, 3);
        splitter.SetCurrentChannel(draw, 1);
        const float clipMin = ImGui::GetWindowDrawList()->GetClipRectMin().y;
        const float clipMax = ImGui::GetWindowDrawList()->GetClipRectMax().y;
        const bool visualDrag = state.dragging && state.current < options.count;
        const float draggedHeight = visualDrag ? state.heights[state.current] : 0;
        size_t first = 0, last = options.count;
        if (options.count)
        {
            const float localMin = clipMin - body.y - g.top - draggedHeight, localMax = clipMax - body.y - g.top + draggedHeight;
            first = static_cast<size_t>(std::upper_bound(state.offsets.begin(), state.offsets.end(), localMin) - state.offsets.begin());
            if (first)
                --first;
            first = (std::min)(first, options.count);
            last = (std::min)(options.count, static_cast<size_t>(std::upper_bound(state.offsets.begin(), state.offsets.end(), localMax) - state.offsets.begin()) + 1);
        }
        float changedHeight = 0;
        bool childActive = false;
        std::vector<size_t> visible;
        visible.reserve(last - first + 1);
        for (size_t i = first; i < last; ++i)
            if (!visualDrag || i != state.current)
                visible.push_back(i);
        // Draw the moving row once, after its neighbors. Repaint it even when
        // its original slot scrolls away; no texture or value snapshot is held.
        if (visualDrag)
            visible.push_back(state.current);
        for (const size_t index : visible)
        {
            if (structureChanged)
                break;
            const bool ghost = visualDrag && index == state.current;
            splitter.SetCurrentChannel(draw, ghost ? 2 : 1);
            const float expected = state.heights[index];
            float target = state.offsets[index] + changedHeight;
            if (visualDrag && !ghost)
            {
                if (state.insertion > state.current && index > state.current && index < state.insertion)
                    target -= draggedHeight;
                if (state.insertion <= state.current && index >= state.insertion && index < state.current)
                    target += draggedHeight;
            }
            auto& animated = state.animatedOffsets[index];
            if (!visualDrag || !std::isfinite(animated))
                animated = target;
            else
                animated += (target - animated) * (1 - std::exp(-20 * ImGui::GetIO().DeltaTime));
            const float position = ghost ? std::clamp(ImGui::GetIO().MousePos.y - body.y - g.top - state.pressPosition.y,
                                                      0.f,
                                                      (std::max)(0.f, state.offsets.back() - expected)) :
                                           animated;
            const ImVec2 row{body.x + 1, body.y + g.top + position};
            const ImVec2 rowMax{body.x + options.width - 1, row.y + expected};
            const bool selected = std::binary_search(state.selection.begin(), state.selection.end(), index);
            if (ghost)
                draw->AddRectFilled({row.x + 2 * options.scale, row.y + 2 * options.scale},
                                    {rowMax.x + 2 * options.scale, rowMax.y + 2 * options.scale},
                                    IM_COL32(0, 0, 0, 90));
            if (selected)
                draw->AddRectFilled(row, rowMax, state.focused ? colors.selection : colors.selectionInactive);
            const bool composite = callbacks.composite && callbacks.composite(index);
            const float handle = options.reorderable && !options.fixedSize ? g.handle : g.padding;
            const float labelWidth = composite ? 0 : (std::min)(options.elementLabelWidth * options.scale, (options.width - handle) * .4f);
            ImGui::PushID(static_cast<int>(index));
            ImGui::SetCursorScreenPos(row);
            ImGui::InvisibleButton("##Handle", {handle, expected});
            const bool hoveredRow = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && Contains(row, rowMax, ImGui::GetIO().MousePos);
            if (!visualDrag && hoveredRow && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                const auto& io = ImGui::GetIO();
                const bool handleHit = io.MousePos.x < row.x + handle;
                if (!selected || ActionKey(io) || io.KeyShift)
                    SelectCollectionRow(state, index, options.count, options.multiSelect && ActionKey(io), options.multiSelect && io.KeyShift);
                else
                    state.current = index;
                focus();
                if (handleHit && options.reorderable && !options.readOnly && !options.fixedSize)
                {
                    state.pressedHandle = true;
                    state.pressPosition = {io.MousePos.x - row.x, io.MousePos.y - row.y};
                    state.dragged = state.selection;
                    result.started = true;
                }
            }
            ImGui::SetCursorScreenPos({row.x + handle, row.y + (g.row - g.header) * .5f});
            if (!composite)
            {
                const auto label = "Element " + std::to_string(index);
                draw->AddText({ImGui::GetCursorScreenPos().x, row.y + (g.row - ImGui::GetFontSize()) * .5f}, ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
                ImGui::SetCursorScreenPos({row.x + handle + labelWidth, row.y + (g.row - g.header) * .5f});
            }
            const float valueWidth = (std::max)(1.f, options.width - handle - labelWidth - g.padding);
            ImGui::SetNextItemWidth(valueWidth);
            ImGui::BeginGroup();
            if (visualDrag)
                ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.f);
            ImGui::BeginDisabled(options.readOnly || visualDrag);
            ControlResult element;
            if (callbacks.drawElement)
                element = callbacks.drawElement(index, valueWidth);
            else
                ImGui::Dummy({valueWidth, g.header});
            ImGui::EndDisabled();
            if (visualDrag)
                ImGui::PopStyleVar();
            ImGui::EndGroup();
            result.Merge(element);
            childActive |= element.active;
            const float actual = (std::max)(g.row, ImGui::GetItemRectMax().y - row.y + (g.row - g.header) * .5f);
            if (options.reorderable && !options.fixedSize)
                Handle(draw, row, g, colors.handle, actual);
            if (ghost)
                draw->AddRect(row, {rowMax.x, row.y + actual}, colors.insertion);
            state.heights[index] = actual;
            changedHeight += actual - expected;
            const ImVec2 actualMax{rowMax.x, row.y + actual};
            const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && Contains(row, actualMax, ImGui::GetIO().MousePos);
            if (!visualDrag && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                focus();
                if (!selected)
                    SelectCollectionRow(state, index, options.count, false, false);
                ImGui::OpenPopup("##ElementMenu");
            }
            if (ImGui::BeginPopup("##ElementMenu"))
            {
                menu(true);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        for (size_t i = 0; i < state.heights.size(); ++i)
            state.offsets[i + 1] = state.offsets[i] + state.heights[i];
        float height = (options.count ? state.offsets.back() : emptyHeight) + g.top + g.bottom;
        splitter.SetCurrentChannel(draw, 0);
        draw->AddRectFilled(body, {body.x + options.width, body.y + height}, colors.background);
        draw->AddRect(body, {body.x + options.width, body.y + height}, colors.border);
        splitter.Merge(draw);
        if (!options.count)
        {
            ImGui::SetCursorScreenPos({body.x + g.padding, body.y + g.top});
            if (options.overMultiEditLimit)
                ImGui::TextWrapped("Multi-object editing is limited to arrays of %zu elements.", options.multiEditLimit);
            else
                ImGui::TextDisabled("List is Empty");
        }
        const auto& io = ImGui::GetIO();
        if (state.pressedHandle && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3 * options.scale))
        {
            state.dragging = true;
            state.dragged = {state.current};
            state.selection = state.dragged;
            state.anchor = state.current;
        }
        if (state.dragging && !structureChanged)
        {
            result.active = true;
            // Match the insertion gap to the floating row, including a grab
            // away from its center. Compare physical pixels so fractional DPI
            // geometry and ImGui's integer mouse coordinates agree.
            const float local = io.MousePos.y - body.y - g.top - state.pressPosition.y + state.heights[state.current] * .5f;
            auto insert = static_cast<size_t>(std::lower_bound(state.offsets.begin(), state.offsets.end(), local) - state.offsets.begin());
            insert = (std::min)(insert, options.count);
            if (insert && insert < state.offsets.size() && std::floor(local + body.y + g.top) < std::floor(body.y + g.top + state.offsets[insert - 1] + state.heights[insert - 1] * .5f))
                --insert;
            state.insertion = insert;
            const float y = body.y + g.top + state.offsets[insert];
            draw->AddLine({body.x, y}, {body.x + options.width, y}, colors.insertion, 2 * options.scale);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (io.MousePos.y < clipMin + g.row)
                ImGui::SetScrollY((std::max)(0.f, ImGui::GetScrollY() - g.row));
            else if (io.MousePos.y > clipMax - g.row)
                ImGui::SetScrollY(ImGui::GetScrollY() + g.row);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                state.dragging = state.pressedHandle = false;
                state.dragged.clear();
                result.canceled = true;
                result.active = false;
            }
            else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                CollectionCommand command{CollectionAction::Move, state.dragged};
                command.insertion = state.insertion;
                execute(std::move(command));
                result.completed = state.error.empty();
                result.canceled |= !state.error.empty();
                state.dragging = state.pressedHandle = false;
                state.dragged.clear();
                result.active = false;
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            state.pressedHandle = false;
        result.active |= state.pressedHandle;

        if (!structureChanged && state.focused && static_cast<ImGuiID>(window->StateStorage.GetInt(focusKey)) == controlId && options.count && !childActive && !state.dragging && !state.editingSize && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        {
            if (static_cast<ImGuiID>(window->StateStorage.GetInt(focusKey)) != controlId)
                state.focused = false;
            size_t next = state.current;
            bool navigate = false;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            {
                next = next ? next - 1 : options.count - 1;
                navigate = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            {
                next = (next + 1) % options.count;
                navigate = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Home))
            {
                next = 0;
                navigate = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_End))
            {
                next = options.count - 1;
                navigate = true;
            }
            if (navigate)
            {
                SelectCollectionRow(state, next, options.count, options.multiSelect && ActionKey(io), options.multiSelect && io.KeyShift);
                ImGui::SetScrollFromPosY(body.y + g.top + state.offsets[next] - ImGui::GetWindowPos().y, .5f);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                execute({CollectionAction::CollapseAll, state.selection});
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                execute({CollectionAction::ExpandAll, state.selection});
            if (ActionKey(io) && ImGui::IsKeyPressed(ImGuiKey_A) && options.multiSelect)
            {
                state.selection.resize(options.count);
                std::iota(state.selection.begin(), state.selection.end(), 0);
            }
            if (!options.readOnly && !options.fixedSize && (io.ConfigMacOSXBehaviors ? ActionKey(io) && ImGui::IsKeyPressed(ImGuiKey_Backspace, false) : ImGui::IsKeyPressed(ImGuiKey_Delete, false)))
                execute({CollectionAction::Remove, state.selection});
            if (ActionKey(io) && ImGui::IsKeyPressed(ImGuiKey_D, false) && !options.readOnly && !options.fixedSize)
                execute({CollectionAction::Duplicate, state.selection});
            if (ActionKey(io) && ImGui::IsKeyPressed(ImGuiKey_C, false))
                execute({CollectionAction::Copy, state.selection});
            if (ActionKey(io) && ImGui::IsKeyPressed(ImGuiKey_V, false) && !options.readOnly)
                execute({CollectionAction::Paste, state.selection});
        }
        if (callbacks.drop && !structureChanged && !options.readOnly && !options.fixedSize && ImGui::BeginDragDropTargetCustom(ImRect(body, {body.x + options.width, body.y + height}), ImGui::GetID("##BodyDrop")))
        {
            drop();
            ImGui::EndDragDropTarget();
        }
        ImGui::SetCursorScreenPos({body.x, body.y + height});
        if (!options.fixedSize)
        {
            const float button = g.footer;
            const ImVec2 foot{body.x + (std::max)(0.f, options.width - button * 2 - 2 * g.padding), body.y + height};
            draw->AddRectFilled(foot, {body.x + options.width, foot.y + g.footer}, colors.footer, 3 * options.scale, ImDrawFlags_RoundCornersBottom);
            draw->AddRect(foot, {body.x + options.width, foot.y + g.footer}, colors.border, 3 * options.scale, ImDrawFlags_RoundCornersBottom);
            ImGui::SetCursorScreenPos({foot.x + g.padding, foot.y});
            ImGui::BeginDisabled(options.readOnly || options.overMultiEditLimit || options.primaryCount >= options.maximumSize);
            if (ImGui::Button("##Add", {button, g.footer}))
                execute({CollectionAction::Add});
            if (ImGui::IsItemActivated())
                focus();
            auto center = ImVec2(ImGui::GetItemRectMin().x + button * .5f, ImGui::GetItemRectMin().y + g.footer * .5f);
            draw->AddLine({center.x - 4 * options.scale, center.y}, {center.x + 4 * options.scale, center.y}, ImGui::GetColorU32(ImGuiCol_Text), options.scale);
            draw->AddLine({center.x, center.y - 4 * options.scale}, {center.x, center.y + 4 * options.scale}, ImGui::GetColorU32(ImGuiCol_Text), options.scale);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Add to list");
            ImGui::EndDisabled();
            ImGui::SameLine(0, 0);
            ImGui::BeginDisabled(options.readOnly || options.count == 0 || options.overMultiEditLimit);
            if (ImGui::Button("##Remove", {button, g.footer}))
                execute({CollectionAction::Remove, state.selection});
            if (ImGui::IsItemActivated())
                focus();
            center = ImVec2(ImGui::GetItemRectMin().x + button * .5f, ImGui::GetItemRectMin().y + g.footer * .5f);
            draw->AddLine({center.x - 4 * options.scale, center.y}, {center.x + 4 * options.scale, center.y}, ImGui::GetColorU32(ImGuiCol_Text), options.scale);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove from list");
            ImGui::EndDisabled();
            ImGui::SetCursorScreenPos({body.x, foot.y + g.footer});
        }
        if (!state.error.empty())
            ImGui::TextColored({1, .45f, .35f, 1}, "%s", state.error.c_str());
    }
    else
        ImGui::SetCursorScreenPos({origin.x, headerMax.y});
    ImGui::EndGroup();
    const auto end = ImGui::GetItemRectMax();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !Contains(origin, {origin.x + options.width, end.y}, ImGui::GetIO().MousePos) && !state.dragging)
    {
        state.focused = false;
        if (static_cast<ImGuiID>(window->StateStorage.GetInt(focusKey)) == controlId)
            window->StateStorage.SetInt(focusKey, 0);
    }
    if (structureChanged || layoutChanged)
    {
        state.heights.clear();
        state.animatedOffsets.clear();
        state.offsets.clear();
    }
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(4);
    ImGui::PopID();
    FinishComposite(result);
    return result;
}
} // namespace NLS::UI::Controls
