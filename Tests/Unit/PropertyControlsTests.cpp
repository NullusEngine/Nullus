#include <imgui_internal.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <UI/Controls/CurveControl.h>
#include <UI/Controls/GradientControl.h>
#include <UI/Controls/NumericExpression.h>
#include <UI/Controls/ResourcePreviewControl.h>
#include <UI/Controls/ScalarControl.h>
#include <UI/Controls/SelectionControl.h>
#include <UI/Controls/TextControl.h>
#include <UI/Controls/VectorControl.h>
#include <memory>

namespace
{
using namespace NLS::UI::Controls;
struct ControlsGui
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ControlsGui()
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1200, 1000};
        io.DeltaTime = 1.f / 60;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~ControlsGui()
    {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    void Begin()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1100, 900});
        ImGui::Begin("Independent controls");
    }
    void End()
    {
        ImGui::End();
        ImGui::Render();
    }
};
} // namespace

TEST(PropertyControls, MatrixKeepsFourRowsWhenItsHostColumnHasANarrowDefaultWidth)
{
    ControlsGui gui;
    gui.Begin();
    const auto start = ImGui::GetCursorScreenPos();
    std::array<float, 16> values{};
    ImGui::PushItemWidth(100);
    ImGui::SetNextItemWidth(500);
    const auto result = Matrix("Four matrix rows", values, 4);
    EXPECT_GT(ImGui::GetItemRectMax().x - start.x, 450.f);
    EXPECT_LE(ImGui::GetItemRectMax().y - start.y, 4 * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y));
    EXPECT_FLOAT_EQ(ImGui::CalcItemWidth(), 100.f);
    EXPECT_FALSE(result.changed);
    ImGui::PopItemWidth();
    gui.End();
}

TEST(PropertyControls, ExpressionsPreserveEveryIntegerBit)
{
    std::string error;
    uint64_t u = 0;
    int64_t s = 0;
    ASSERT_TRUE(EvaluateExpressionAs("18446744073709551615", u, error)) << error;
    EXPECT_EQ(u, UINT64_MAX);
    ASSERT_TRUE(EvaluateExpressionAs("18446744073709551615 - 2", u, error)) << error;
    EXPECT_EQ(u, UINT64_MAX - 2);
    ASSERT_TRUE(EvaluateExpressionAs("-9223372036854775808", s, error)) << error;
    EXPECT_EQ(s, INT64_MIN);
    EXPECT_TRUE(EvaluateExpressionAs("(9007199254740993 + 7) / 2", s, error));
    EXPECT_EQ(s, 4503599627370500ll);
    u = 71;
    EXPECT_FALSE(EvaluateExpressionAs("18446744073709551615 + 1", u, error));
    EXPECT_EQ(u, 71u);
    EXPECT_FALSE(EvaluateExpressionAs("-1", u, error));
    EXPECT_EQ(u, 71u);
    EXPECT_FALSE(EvaluateExpressionAs("1 / 2", s, error));
    EXPECT_FALSE(EvaluateExpressionAs("1.00000000000000001", s, error));
    ASSERT_TRUE(EvaluateExpressionAs("min(18446744073709551614,18446744073709551615)", u, error));
    EXPECT_EQ(u, UINT64_MAX - 1);
}
TEST(PropertyControls, ExpressionsValidateFunctionsAndErrors)
{
    std::string error;
    double v = 0;
    ASSERT_TRUE(EvaluateExpressionAs("2 * (3 + sin(pi/2)) + sqrt(9)", v, error));
    EXPECT_NEAR(v, 11, 1e-12);
    ASSERT_TRUE(EvaluateExpressionAs("clamp(pow(2,3),1,6)", v, error));
    EXPECT_DOUBLE_EQ(v, 6);
    for (const char* text : {"1/0", "sqrt(-1)", "exp(100000)", "missing(3)", "1 trailing", "(3", "", "min(1)"})
        EXPECT_FALSE(EvaluateExpressionAs(text, v, error)) << text;
    int8_t small = 0;
    EXPECT_FALSE(EvaluateExpressionAs("128", small, error));
    float f = 0;
    EXPECT_FALSE(EvaluateExpressionAs("1e100", f, error));
}
TEST(PropertyControls, CurveMovementPreservesOrderingAndUneditedChannels)
{
    CurveData curve;
    curve.channels = 2;
    curve.keys.resize(3);
    curve.keys[0].time = 0;
    curve.keys[1].time = 1;
    curve.keys[2].time = 2;
    curve.keys[1].value = {4, 8, 0, 0};
    const size_t selected[]{1};
    EXPECT_TRUE(MoveCurveKeys(curve, selected, 5, 2, 0));
    EXPECT_LT(curve.keys[1].time, curve.keys[2].time);
    EXPECT_FLOAT_EQ(curve.keys[1].value[0], 6);
    EXPECT_FLOAT_EQ(curve.keys[1].value[1], 8);
    const auto before = curve;
    const size_t invalid[]{4};
    EXPECT_FALSE(MoveCurveKeys(curve, invalid, 1, 1, 0));
    EXPECT_EQ(curve, before);
}
TEST(PropertyControls, CurveEvaluationAndViewportRoundTrip)
{
    CurveData curve;
    curve.keys.resize(2);
    curve.keys[0].interpolation = 0;
    curve.keys[1].time = 2;
    curve.keys[1].value[0] = 4;
    EXPECT_FLOAT_EQ(EvaluateCurve(curve, 1), 2);
    curve.keys[0].interpolation = 1;
    curve.keys[0].outTangent[0] = curve.keys[1].inTangent[0] = 2;
    curve.weights = true;
    curve.keys[0].weightedMode = 2;
    curve.keys[1].weightedMode = 1;
    curve.keys[0].outWeight = .2f;
    curve.keys[1].inWeight = .4f;
    EXPECT_NEAR(EvaluateCurve(curve, 1), 2, 1e-5);
    CurveViewport view;
    view.Fit(curve, 0);
    view.Zoom(.7, 1.5f, .6f);
    EXPECT_NEAR(view.Time(view.X(.8, 640), 640), .8, 1e-6);
    EXPECT_NEAR(view.Value(view.Y(2.3f, 280), 280), 2.3f, 1e-6);
}
TEST(PropertyControls, GradientKeepsColorAndAlphaTracksIndependent)
{
    GradientData gradient;
    gradient.colors = {{0, {2, 0, 0, .37f}}, {1, {0, 0, 4, .91f}}};
    gradient.alphas = {{0, 0}, {1, 1}};
    auto value = EvaluateGradient(gradient, .25f);
    EXPECT_FLOAT_EQ(value[0], 1.5f);
    EXPECT_FLOAT_EQ(value[2], 1);
    EXPECT_FLOAT_EQ(value[3], .25f);
    gradient.mode = 1;
    value = EvaluateGradient(gradient, .75f);
    EXPECT_FLOAT_EQ(value[0], 2);
    EXPECT_FLOAT_EQ(value[2], 0);
    EXPECT_FLOAT_EQ(value[3], 0);
}
TEST(PropertyControls, PreviewRejectsWrongIdentityRevisionGenerationAndClosedHost)
{
    auto lifetime = std::make_shared<int>();
    PreviewRequest request{"asset-a", 3, 128, lifetime};
    ResourcePreviewState state;
    PreviewImage frame;
    frame.identity = "asset-a";
    frame.revision = 3;
    frame.generation = 7;
    ASSERT_TRUE(AcceptPreviewResult(state, request, frame));
    frame.identity = "asset-b";
    EXPECT_FALSE(AcceptPreviewResult(state, request, frame));
    frame.identity = "asset-a";
    frame.revision = 2;
    EXPECT_FALSE(AcceptPreviewResult(state, request, frame));
    frame.revision = 3;
    frame.generation = 6;
    EXPECT_FALSE(AcceptPreviewResult(state, request, frame));
    lifetime.reset();
    frame.generation = 8;
    EXPECT_FALSE(AcceptPreviewResult(state, request, frame));
    EXPECT_EQ(state.image->generation, 7u);
}
TEST(PropertyControls, EveryControlDrawsWithoutAnEditorAtAllSupportedScales)
{
    ControlsGui gui;
    std::string text(12000, 'x');
    uint64_t integer = UINT64_MAX;
    bool toggle = true;
    float vector[]{1, 2, 3}, rgba[]{2, .3f, .2f, 1};
    CurveData curve;
    curve.keys.resize(2);
    curve.keys.back().time = 1;
    CurveState curveState;
    GradientData gradient;
    GradientState gradientState;
    SelectionItem item[]{{"First"}, {"Second"}};
    SelectionState selectionState;
    int selected = 1;
    for (float scale : {1.f, 1.5f, 2.f})
    {
        gui.Begin();
        ControlOptions options{false, false, scale};
        EXPECT_FALSE(Text("Text", text, false, options).changed);
        EXPECT_EQ(text.size(), 12000u);
        EXPECT_FALSE(Numeric("Exact integer", integer, {}, options).changed);
        EXPECT_EQ(integer, UINT64_MAX);
        EXPECT_FALSE(Toggle("Toggle", toggle, options).changed);
        EXPECT_FALSE(FloatComponents("Vector", vector, {}, .01f, options).changed);
        EXPECT_FALSE(Color("HDR color", rgba, true, options).changed);
        EXPECT_FALSE(Selection("Choice", selected, item, selectionState, options).changed);
        EXPECT_FALSE(Curve("Curve", curve, curveState, options).changed);
        EXPECT_FALSE(Gradient("Gradient", gradient, gradientState, options).changed);
        gui.End();
    }
}
TEST(PropertyControls, CompositeCompletionWaitsForAllChildren)
{
    ControlResult result;
    result.Merge({true, false, true, true, false, 1});
    result.Merge({false, true, false, false, false, 0});
    FinishComposite(result);
    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(result.active);
    EXPECT_FALSE(result.completed);
}

TEST(PropertyControls, TextGrowsDuringRealInputAndEscapeCancels)
{
    ControlsGui gui;
    std::string text;
    ImVec2 hit{};
    const auto frame = [&]
    {
        gui.Begin();
        auto result = Text("Dynamic input", text);
        const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        hit = {(a.x + b.x) * .5f, (a.y + b.y) * .5f};
        gui.End();
        return result;
    };
    frame();
    frame();
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(hit.x, hit.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    const std::string input(6000, 'q');
    io.AddInputCharactersUTF8(input.c_str());
    const auto edited = frame();
    EXPECT_TRUE(edited.changed);
    EXPECT_EQ(text, input);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    const auto canceled = frame();
    EXPECT_TRUE(canceled.canceled);
    EXPECT_FALSE(canceled.completed);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    frame();
    EXPECT_TRUE(text.empty());
}
TEST(PropertyControls, DragAggregatesContinuousChangesUntilMouseRelease)
{
    ControlsGui gui;
    float value = 0;
    ImVec2 hit{};
    const auto frame = [&]
    {
        gui.Begin();
        ScalarOptions options;
        options.drag = true;
        options.speed = 1;
        auto result = Numeric("Drag value", value, options);
        const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        hit = {(a.x + b.x) * .5f, (a.y + b.y) * .5f};
        gui.End();
        return result;
    };
    frame();
    frame();
    const auto start = hit;
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(start.x, start.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMousePosEvent(start.x + 40, start.y);
    const auto moved = frame();
    EXPECT_TRUE(moved.active);
    EXPECT_FALSE(moved.completed);
    io.AddMousePosEvent(start.x + 80, start.y);
    const auto movedAgain = frame();
    EXPECT_TRUE(movedAgain.changed);
    EXPECT_GT(value, 0);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    const auto ended = frame();
    EXPECT_TRUE(ended.completed);
    EXPECT_FALSE(ended.active);
}

TEST(PropertyControls, CurvePreviewHonorsLoopAndPingPongWrap)
{
    CurveData curve;
    curve.keys.resize(2);
    curve.keys[0].interpolation = 0;
    curve.keys[1].time = 1;
    curve.keys[1].value[0] = 1;
    curve.wrap = true;
    curve.preWrap = curve.postWrap = 2;
    EXPECT_NEAR(EvaluateCurve(curve, 1.25), .25f, 1e-6);
    EXPECT_NEAR(EvaluateCurve(curve, -.25), .75f, 1e-6);
    curve.postWrap = 4;
    EXPECT_NEAR(EvaluateCurve(curve, 1.25), .75f, 1e-6);
}

TEST(PropertyControls, BoundedTextKeepsUtf8CharactersWhole)
{
    ControlsGui gui;
    std::string value;
    ImVec2 hit{};
    const auto frame = [&]
    {gui.Begin();auto result=Text("Bounded",value,false,{}, {},0,3);const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();hit={(a.x+b.x)*.5f,(a.y+b.y)*.5f};gui.End();return result; };
    frame();
    frame();
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(hit.x, hit.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    io.AddInputCharactersUTF8("\xc3\xa9\xc3\xa9");
    frame();
    EXPECT_EQ(value, "\xc3\xa9");
}
TEST(PropertyControls, PassiveTextDrawPreservesEmbeddedNullBytes)
{
    ControlsGui gui;
    std::string value("left\0right", 10);
    const auto original = value;
    gui.Begin();
    EXPECT_FALSE(Text("Opaque string", value).changed);
    gui.End();
    EXPECT_EQ(value, original);
}

TEST(PropertyControls, FlagPopupCompletesOneGestureWhenClosed)
{
    ControlsGui gui;
    uint64_t value = 0;
    const uint64_t masks[]{1, 0x8000000000000000ull};
    const SelectionItem items[]{{"Low"}, {"High"}};
    ImVec2 minimum{}, maximum{}, firstItem{};
    const auto frame = [&]
    {gui.Begin();const auto result=Flags("Bits",value,masks,items);if(!result.active) {minimum=ImGui::GetItemRectMin();maximum=ImGui::GetItemRectMax();}else {auto& popups=ImGui::GetCurrentContext()->OpenPopupStack;if(!popups.empty() && popups.back().Window) {const auto start=popups.back().Window->DC.CursorStartPos;firstItem={start.x+10,start.y+ImGui::GetTextLineHeight()*.5f};}}gui.End();return result; };
    frame();
    frame();
    auto& io = ImGui::GetIO();
    const auto rootMin = minimum, rootMax = maximum;
    io.AddMousePosEvent((rootMin.x + rootMax.x) * .5f, (rootMin.y + rootMax.y) * .5f);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    const auto opened = frame();
    ASSERT_TRUE(opened.active);
    frame();
    frame();
    io.AddMousePosEvent(firstItem.x, firstItem.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    const auto picked = frame();
    ASSERT_EQ(value, 1u) << "popup item=" << firstItem.x << "," << firstItem.y << " mouse=" << io.MousePos.x << "," << io.MousePos.y << " active=" << ImGui::GetCurrentContext()->ActiveId << " hovered=" << ImGui::GetCurrentContext()->HoveredId;
    EXPECT_TRUE(picked.changed);
    EXPECT_TRUE(picked.active);
    EXPECT_FALSE(picked.completed);
    io.AddMousePosEvent(1000, 800);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    const auto pressed = frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    const auto released = frame();
    const auto closed = frame();
    EXPECT_EQ(static_cast<int>(pressed.completed) + static_cast<int>(released.completed) + static_cast<int>(closed.completed), 1);
    EXPECT_FALSE(closed.active);
}
