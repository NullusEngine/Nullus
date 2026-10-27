#include <gtest/gtest.h>
#include <imgui_internal.h>
#include <UI/Controls/CollectionControl.h>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <cmath>
#include <UI/Controls/ScalarControl.h>

namespace
{
using namespace NLS::UI::Controls;
struct CollectionGui
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    CollectionState state;
    CollectionOptions options;
    CollectionCallbacks callbacks;
    std::vector<CollectionCommand> commands;
    ImVec2 origin{};
    size_t rows = 0;
    std::function<void()> after;
    CollectionGui()
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1000, 900};
        io.DeltaTime = 1.f / 60;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels;
        int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        options.label = "Values";
        options.count = options.primaryCount = 6;
        options.width = 460;
        state.expanded = true;
        callbacks.drawElement = [&](size_t, float width)
        { ++rows; ImGui::Dummy({width,18*options.scale}); return ControlResult{}; };
        callbacks.execute = [&](const CollectionCommand& command)
        {
            commands.push_back(command);
            CollectionCommandResult result;
            result.count = options.count;
            result.revision = options.revision;
            result.selection = command.indices;
            if (command.action == CollectionAction::Add)
            {
                ++result.count;
                result.selection = {result.count - 1};
            }
            if (command.action == CollectionAction::Remove)
                result.count -= command.indices.empty() ? 1 : command.indices.size();
            if (command.action == CollectionAction::Resize)
                result.count = command.size;
            result.changed = command.action == CollectionAction::Move || result.count != options.count;
            if (result.changed)
            {
                options.count = options.primaryCount = result.count;
                result.revision = ++options.revision;
            }
            return result;
        };
        Frame();
        Frame();
    }
    ~CollectionGui()
    {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    ControlResult Frame()
    {
        rows = 0;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({800, 750});
        ImGui::Begin("Collection test", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        origin = ImGui::GetCursorScreenPos();
        auto result = Collection("list", state, options, callbacks);
        if (after)
            after();
        ImGui::End();
        ImGui::Render();
        return result;
    }
    ImVec2 Handle(size_t index) const
    {
        const auto g = GetCollectionGeometry(options.scale, ImGui::GetFontSize());
        return {origin.x + g.handle * .5f, origin.y + g.header + g.headerGap + g.top + index * g.row + g.row * .5f};
    }
    ControlResult Mouse(ImVec2 p, bool down)
    {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(p.x, p.y);
        io.AddMouseButtonEvent(0, down);
        return Frame();
    }
    void Click(size_t i)
    {
        Mouse(Handle(i), true);
        Mouse(Handle(i), false);
    }
    ControlResult Key(ImGuiKey key, bool down = true)
    {
        ImGui::GetIO().AddKeyEvent(key, down);
        return Frame();
    }
};
} // namespace

TEST(CollectionControl, SelectionSupportsToggleAndAnchoredRanges)
{
    CollectionState s;
    SelectCollectionRow(s, 2, 10, false, false);
    SelectCollectionRow(s, 5, 10, false, true);
    EXPECT_EQ(s.selection, (std::vector<size_t>{2, 3, 4, 5}));
    SelectCollectionRow(s, 3, 10, true, false);
    EXPECT_EQ(s.selection, (std::vector<size_t>{2, 4, 5}));
    SelectCollectionRow(s, 20, 10, false, false);
    EXPECT_EQ(s.current, 3u);
}
TEST(CollectionControl, MovePermutationHandlesEveryGapAndRejectsInvalidCommands)
{
    const size_t block[]{1, 3};
    EXPECT_EQ(CollectionMoveOrder(5, block, 5), (std::vector<size_t>{0, 2, 4, 1, 3}));
    EXPECT_EQ(CollectionMoveOrder(5, block, 0), (std::vector<size_t>{1, 3, 0, 2, 4}));
    const size_t single[]{2};
    EXPECT_EQ(CollectionMoveOrder(5, single, 3), (std::vector<size_t>{0, 1, 2, 3, 4}));
    const size_t invalid[]{1, 1};
    EXPECT_TRUE(CollectionMoveOrder(5, invalid, 2).empty());
    EXPECT_TRUE(CollectionMoveOrder(5, block, 6).empty());
}
TEST(CollectionControl, RevisionChangeCancelsDragAndSizeButValueRefreshPreservesLayout)
{
    CollectionState s;
    CollectionOptions o;
    o.count = 3;
    o.width = 300;
    o.revision = 4;
    SynchronizeCollection(s, o);
    s.heights = {21, 45, 21};
    s.dragging = s.pressedHandle = s.editingSize = true;
    SynchronizeCollection(s, o);
    EXPECT_TRUE(s.dragging);
    EXPECT_EQ(s.heights[1], 45);
    ++o.revision;
    SynchronizeCollection(s, o);
    EXPECT_FALSE(s.dragging);
    EXPECT_FALSE(s.editingSize);
    EXPECT_EQ(s.heights[1], 0);
    s.heights = {21, 45, 21};
    s.dragging = s.pressedHandle = true;
    o.readOnly = true;
    SynchronizeCollection(s, o);
    EXPECT_FALSE(s.dragging);
    EXPECT_FALSE(s.pressedHandle);
    EXPECT_EQ(s.heights[1], 45);
}
TEST(CollectionControl, RealInputSupportsCtrlShiftSelectionAndWrappedKeyboardNavigation)
{
    CollectionGui gui;
    gui.Click(1);
    gui.Key(ImGuiMod_Ctrl);
    gui.Click(4);
    gui.Key(ImGuiMod_Ctrl, false);
    EXPECT_EQ(gui.state.selection, (std::vector<size_t>{1, 4}));
    gui.Key(ImGuiMod_Shift);
    gui.Click(2);
    gui.Key(ImGuiMod_Shift, false);
    EXPECT_EQ(gui.state.selection, (std::vector<size_t>{2, 3, 4}));
    gui.Click(0);
    gui.Key(ImGuiKey_UpArrow);
    gui.Key(ImGuiKey_UpArrow, false);
    EXPECT_EQ(gui.state.selection, (std::vector<size_t>{5}));
    gui.Key(ImGuiKey_DownArrow);
    gui.Key(ImGuiKey_DownArrow, false);
    EXPECT_EQ(gui.state.selection, (std::vector<size_t>{0}));
    EXPECT_TRUE(gui.commands.empty());
}
TEST(CollectionControl, RealDragEmitsOneMoveOnlyAfterRelease)
{
    CollectionGui gui;
    gui.Mouse(gui.Handle(1), true);
    auto destination = gui.Handle(4);
    destination.y += 8;
    gui.Mouse(destination, true);
    EXPECT_TRUE(gui.state.dragging);
    EXPECT_TRUE(gui.commands.empty());
    gui.Mouse(destination, true);
    EXPECT_TRUE(gui.commands.empty());
    gui.Mouse(destination, false);
    ASSERT_EQ(gui.commands.size(), 1u);
    EXPECT_EQ(gui.commands[0].action, CollectionAction::Move);
    EXPECT_EQ(gui.commands[0].indices, (std::vector<size_t>{1}));
    EXPECT_EQ(gui.commands[0].insertion, 5u);
}
TEST(CollectionControl, EscapeCancelsRealDragWithoutCommand)
{
    CollectionGui gui;
    gui.Mouse(gui.Handle(1), true);
    gui.Mouse(gui.Handle(4), true);
    ASSERT_TRUE(gui.state.dragging);
    EXPECT_TRUE(gui.Key(ImGuiKey_Escape).canceled);
    gui.Mouse(gui.Handle(4), false);
    gui.Key(ImGuiKey_Escape, false);
    EXPECT_FALSE(gui.state.dragging);
    EXPECT_TRUE(gui.commands.empty());
}
TEST(CollectionControl, DeleteIsOneCommandAndReadonlyProtectsMutation)
{
    CollectionGui gui;
    gui.Click(2);
    gui.Key(ImGuiKey_Delete);
    gui.Key(ImGuiKey_Delete, false);
    ASSERT_EQ(gui.commands.size(), 1u);
    EXPECT_EQ(gui.options.count, 5u);
    gui.options.readOnly = true;
    gui.Click(1);
    gui.Key(ImGuiKey_Delete);
    gui.Key(ImGuiKey_Delete, false);
    EXPECT_EQ(gui.commands.size(), 1u);
}

TEST(CollectionControl, MacInputUsesCommandSelectionAndCommandBackspace)
{
    CollectionGui gui;
    ImGui::GetIO().ConfigMacOSXBehaviors = true;
    gui.Click(1);
    gui.Key(ImGuiMod_Super);
    gui.Click(4);
    EXPECT_EQ(gui.state.selection, (std::vector<size_t>{1, 4}));
    gui.Key(ImGuiKey_Backspace);
    gui.Key(ImGuiKey_Backspace, false);
    gui.Key(ImGuiMod_Super, false);
    ASSERT_EQ(gui.commands.size(), 1u);
    EXPECT_EQ(gui.commands[0].action, CollectionAction::Remove);
    EXPECT_EQ(gui.commands[0].indices, (std::vector<size_t>{1, 4}));
}
TEST(CollectionControl, LargeCollectionsCullRowsAndScaleAtAllRequestedDpis)
{
    CollectionGui gui;
    gui.options.count = gui.options.primaryCount = 10000;
    for (const float scale : {1.f, 1.5f, 2.f})
    {
        gui.options.scale = scale;
        gui.Frame();
        EXPECT_LT(gui.rows, 50u);
        EXPECT_GT(gui.rows, 0u);
        const auto g = GetCollectionGeometry(scale, 13 * scale);
        EXPECT_FLOAT_EQ(g.row, 21 * scale);
        EXPECT_FLOAT_EQ(g.sizeWidth, 48 * scale);
    }
}
TEST(CollectionControl, VariableHeightRowsAreMeasuredWithoutRebuildingStableState)
{
    CollectionGui gui;
    gui.callbacks.composite = [](size_t)
    { return true; };
    gui.callbacks.drawElement = [](size_t i, float width)
    {ImGui::Dummy({width,i==1?120.f:18.f});return ControlResult{}; };
    gui.Frame();
    gui.Frame();
    ASSERT_EQ(gui.state.heights.size(), 6u);
    EXPECT_GT(gui.state.heights[1], 110);
    EXPECT_GT(gui.state.offsets[2] - gui.state.offsets[1], 110);
}

TEST(CollectionControl, FooterButtonsAndSizeFieldUseSingleCompletedCommands)
{
    CollectionGui gui;
    const auto g = GetCollectionGeometry(1, ImGui::GetFontSize());
    ImVec2 plus{gui.origin.x + gui.options.width - 2 * g.footer - g.padding + g.footer * .5f,
                gui.origin.y + g.header + g.headerGap + g.top + gui.state.offsets.back() + g.bottom + g.footer * .5f};
    gui.Mouse(plus, true);
    gui.Mouse(plus, false);
    ASSERT_EQ(gui.commands.size(), 1u);
    EXPECT_EQ(gui.commands[0].action, CollectionAction::Add);
    EXPECT_EQ(gui.options.count, 7u);
    ImVec2 size{gui.origin.x + gui.options.width - 24, gui.origin.y + 9};
    gui.Mouse(size, true);
    gui.Mouse(size, false);
    gui.Key(ImGuiMod_Ctrl);
    gui.Key(ImGuiKey_A);
    gui.Key(ImGuiKey_A, false);
    gui.Key(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddInputCharactersUTF8("12");
    gui.Frame();
    EXPECT_EQ(gui.commands.size(), 1u);
    gui.Key(ImGuiKey_Enter);
    gui.Key(ImGuiKey_Enter, false);
    ASSERT_EQ(gui.commands.size(), 2u);
    EXPECT_EQ(gui.commands[1].action, CollectionAction::Resize);
    EXPECT_EQ(gui.options.count, 12u);
}

TEST(CollectionControl, FooterAndSizeFocusRouteKeyboardToOnlyTheOperatedList)
{
    CollectionGui gui;
    CollectionState other;
    other.expanded = true;
    CollectionOptions options;
    options.label = "Other";
    options.count = options.primaryCount = 3;
    options.width = 460;
    CollectionCallbacks callbacks;
    std::vector<CollectionAction> actions;
    callbacks.drawElement = [](size_t, float width)
    {ImGui::Dummy({width,18});return ControlResult{}; };
    callbacks.execute = [&](const CollectionCommand& command)
    {
        actions.push_back(command.action);
        if (command.action == CollectionAction::Add)
            ++options.count;
        if (command.action == CollectionAction::Remove)
            --options.count;
        options.primaryCount = options.count;
        return CollectionCommandResult{true, options.count, ++options.revision};
    };
    ImVec2 origin;
    gui.after = [&]
    {origin=ImGui::GetCursorScreenPos();Collection("other",other,options,callbacks); };
    gui.Frame();
    gui.Frame();
    gui.Click(1);
    const auto g = GetCollectionGeometry(1, ImGui::GetFontSize());
    const ImVec2 plus{origin.x + options.width - 2 * g.footer - g.padding + g.footer * .5f,
                      origin.y + g.header + g.headerGap + g.top + other.offsets.back() + g.bottom + g.footer * .5f};
    gui.Mouse(plus, true);
    gui.Mouse(plus, false);
    gui.Key(ImGuiKey_Delete);
    gui.Key(ImGuiKey_Delete, false);
    EXPECT_TRUE(gui.commands.empty());
    EXPECT_EQ(options.count, 3u);
    EXPECT_EQ(actions, (std::vector<CollectionAction>{CollectionAction::Add, CollectionAction::Remove}));
    gui.Click(1);
    const ImVec2 count{origin.x + options.width - 24, origin.y + 9};
    gui.Mouse(count, true);
    gui.Mouse(count, false);
    gui.Key(ImGuiKey_Escape);
    gui.Key(ImGuiKey_Escape, false);
    gui.Key(ImGuiKey_Delete);
    gui.Key(ImGuiKey_Delete, false);
    EXPECT_TRUE(gui.commands.empty());
    EXPECT_EQ(options.count, 2u);
    EXPECT_EQ(gui.options.count, 6u);
}

TEST(CollectionControl, MixedSizeCanConfirmThePrimaryNumberAndUnchangedInputStillCompletes)
{
    CollectionGui gui;
    gui.options.count = 3;
    gui.options.primaryCount = 6;
    gui.options.mixedSize = true;
    gui.Frame();
    const ImVec2 count{gui.origin.x + gui.options.width - 24, gui.origin.y + 9};
    gui.Mouse(count, true);
    gui.Mouse(count, false);
    gui.Key(ImGuiMod_Ctrl);
    gui.Key(ImGuiKey_A);
    gui.Key(ImGuiKey_A, false);
    gui.Key(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddInputCharactersUTF8("6");
    gui.Frame();
    const auto confirmed = gui.Key(ImGuiKey_Enter);
    gui.Key(ImGuiKey_Enter, false);
    ASSERT_EQ(gui.commands.size(), 1u);
    EXPECT_EQ(gui.commands[0].action, CollectionAction::Resize);
    EXPECT_EQ(gui.commands[0].size, 6u);
    EXPECT_TRUE(confirmed.changed);
    EXPECT_TRUE(confirmed.completed);
    EXPECT_FALSE(confirmed.active);
    gui.options.mixedSize = false;
    gui.Frame();
    gui.Mouse(count, true);
    gui.Mouse(count, false);
    const auto unchanged = gui.Key(ImGuiKey_Enter);
    gui.Key(ImGuiKey_Enter, false);
    EXPECT_FALSE(unchanged.changed);
    EXPECT_TRUE(unchanged.completed);
    EXPECT_FALSE(unchanged.active);
}

TEST(CollectionControl, DragPreviewFollowsThePointerAndRepaintsItsOffscreenSourceOnce)
{
    CollectionGui gui;
    gui.options.count = gui.options.primaryCount = 10000;
    gui.Frame();
    gui.Frame();
    std::vector<size_t> calls;
    ImVec2 moving{};
    gui.callbacks.drawElement = [&](size_t i, float width)
    {calls.push_back(i);if(i==1) moving=ImGui::GetCursorScreenPos();ImGui::Dummy({width,18});return ControlResult{}; };
    gui.Mouse(gui.Handle(1), true);
    const auto original = moving;
    const auto destination = gui.Handle(7);
    gui.Mouse(destination, true);
    calls.clear();
    gui.Mouse(destination, true);
    ASSERT_TRUE(gui.state.dragging);
    EXPECT_EQ(calls.back(), 1u);
    EXPECT_EQ(std::count(calls.begin(), calls.end(), 1u), 1);
    EXPECT_GT(moving.y, original.y + 60);
    auto* window = ImGui::FindWindowByName("Collection test");
    ASSERT_NE(window, nullptr);
    window->Scroll.y = 600;
    calls.clear();
    gui.Mouse(destination, true);
    EXPECT_EQ(calls.back(), 1u);
    EXPECT_EQ(std::count(calls.begin(), calls.end(), 1u), 1);
    EXPECT_LT(calls.size(), 80u);
    EXPECT_GE(moving.y, window->InnerClipRect.Min.y);
    EXPECT_LT(moving.y, window->InnerClipRect.Max.y);
    gui.Key(ImGuiKey_Escape);
    gui.Mouse(destination, false);
    EXPECT_TRUE(gui.commands.empty());
}

namespace
{
// Rasterize actual ImGui draw data, including the font atlas. This makes the
// optional review artifacts independent of a window server and GPU backend.
void CaptureCollection(const std::filesystem::path& path, int width, int height)
{
    unsigned char* atlas;
    int tw, th;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas, &tw, &th);
    std::vector<unsigned char> rgb(static_cast<size_t>(width) * height * 3, 32);
    const auto* data = ImGui::GetDrawData();
    for (int list = 0; list < data->CmdListsCount; ++list)
    {
        const auto* draw = data->CmdLists[list];
        for (const auto& cmd : draw->CmdBuffer)
        {
            if (cmd.UserCallback)
                continue;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3)
            {
                const auto a = draw->VtxBuffer[draw->IdxBuffer[cmd.IdxOffset + i] + cmd.VtxOffset];
                const auto b = draw->VtxBuffer[draw->IdxBuffer[cmd.IdxOffset + i + 1] + cmd.VtxOffset];
                const auto c = draw->VtxBuffer[draw->IdxBuffer[cmd.IdxOffset + i + 2] + cmd.VtxOffset];
                const float area = (b.pos.y - c.pos.y) * (a.pos.x - c.pos.x) + (c.pos.x - b.pos.x) * (a.pos.y - c.pos.y);
                if (std::abs(area) < .00001f)
                    continue;
                const int x0 = (std::max)(0, static_cast<int>(std::floor((std::max)(cmd.ClipRect.x, (std::min)({a.pos.x, b.pos.x, c.pos.x})))));
                const int x1 = (std::min)(width, static_cast<int>(std::ceil((std::min)(cmd.ClipRect.z, (std::max)({a.pos.x, b.pos.x, c.pos.x})))));
                const int y0 = (std::max)(0, static_cast<int>(std::floor((std::max)(cmd.ClipRect.y, (std::min)({a.pos.y, b.pos.y, c.pos.y})))));
                const int y1 = (std::min)(height, static_cast<int>(std::ceil((std::min)(cmd.ClipRect.w, (std::max)({a.pos.y, b.pos.y, c.pos.y})))));
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                    {
                        const float u = ((b.pos.y - c.pos.y) * (x + .5f - c.pos.x) + (c.pos.x - b.pos.x) * (y + .5f - c.pos.y)) / area;
                        const float v = ((c.pos.y - a.pos.y) * (x + .5f - c.pos.x) + (a.pos.x - c.pos.x) * (y + .5f - c.pos.y)) / area;
                        const float w = 1 - u - v;
                        if (u < 0 || v < 0 || w < 0)
                            continue;
                        const int tx = std::clamp(static_cast<int>((u * a.uv.x + v * b.uv.x + w * c.uv.x) * tw), 0, tw - 1);
                        const int ty = std::clamp(static_cast<int>((u * a.uv.y + v * b.uv.y + w * c.uv.y) * th), 0, th - 1);
                        const float alpha = (u * (a.col >> 24) + v * (b.col >> 24) + w * (c.col >> 24)) * atlas[(ty * tw + tx) * 4 + 3] / 65025.f;
                        for (int channel = 0; channel < 3; ++channel)
                        {
                            const float color = u * ((a.col >> (channel * 8)) & 255) + v * ((b.col >> (channel * 8)) & 255) + w * ((c.col >> (channel * 8)) & 255);
                            auto& dest = rgb[(static_cast<size_t>(y) * width + x) * 3 + channel];
                            dest = static_cast<unsigned char>(color * alpha + dest * (1 - alpha));
                        }
                    }
            }
        }
    }
    std::ofstream file(path, std::ios::binary);
    file << "P6\n"
         << width << ' ' << height << "\n255\n";
    file.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
}
} // namespace

TEST(CollectionControl, RenderBothThemesAtAllRequestedDpis)
{
    for (const bool light : {false, true})
        for (const float dpi : {1.f, 1.5f, 2.f})
        {
            CollectionGui gui;
            if (light)
                ImGui::StyleColorsLight();
            else
                ImGui::StyleColorsDark();
            gui.options.scale = dpi;
            gui.options.width = 680;
            gui.options.label = "Array / List";
            ImGui::GetIO().FontGlobalScale = dpi;
            std::vector<float> values{1, 2, 3, 4, 5, 6};
            std::vector<ImVec2> positions(values.size());
            gui.callbacks.drawElement = [&](size_t i, float)
            { positions[i]=ImGui::GetCursorScreenPos();return Numeric("##Value", values[i]); };
            gui.state.selection = {1, 2};
            gui.Frame();
            gui.Frame();
            EXPECT_GT(gui.state.offsets.back(), 100 * dpi);
            if (const char* directory = std::getenv("NLS_COLLECTION_CAPTURE_DIR"))
            {
                std::filesystem::create_directories(directory);
                const std::string name = std::string(light ? "light-" : "dark-") + std::to_string(static_cast<int>(dpi * 100)) + ".ppm";
                CaptureCollection(std::filesystem::path(directory) / name, 800, static_cast<int>(100 + 180 * dpi));
                const auto capture = [&](const char* suffix)
                {
                    const auto filename = std::string(light ? "light-" : "dark-") + std::to_string(static_cast<int>(dpi * 100)) + suffix + ".ppm";
                    CaptureCollection(std::filesystem::path(directory) / filename, 800, static_cast<int>(100 + 180 * dpi));
                };
                gui.Mouse(gui.Handle(1), true);
                gui.Mouse(gui.Handle(4), true);
                for (int frame = 0; frame < 12; ++frame)
                    gui.Frame();
                EXPECT_EQ(gui.state.insertion, 5u);
                capture("-drag");
                std::ofstream layout(std::filesystem::path(directory) / (name + ".layout.txt"));
                for (size_t i = 0; i < positions.size(); ++i)
                    layout << i << " y=" << positions[i].y << " h=" << gui.state.heights[i] << " offset=" << gui.state.offsets[i] << " animated=" << gui.state.animatedOffsets[i] << "\n";
                layout << "insertion=" << gui.state.insertion << "\n";
                gui.Key(ImGuiKey_Escape);
                gui.Mouse(gui.Handle(4), false);
                gui.Key(ImGuiKey_Escape, false);
                gui.options.mixedSize = true;
                gui.Frame();
                capture("-mixed");
            }
        }
}
