#include <gtest/gtest.h>

#include <cstdint>
#include <functional>

#include "Debug/Logger.h"
#include "Rendering/UI/UiDrawDataSnapshot.h"
#include "ImGui/imgui.h"

namespace
{
    bool g_unsupportedCallbackExecuted = false;

    void UnsupportedDrawCallback(const ImDrawList*, const ImDrawCmd*)
    {
        g_unsupportedCallbackExecuted = true;
    }

    struct ImGuiContextGuard
    {
        ImGuiContextGuard()
        {
            IMGUI_CHECKVERSION();
            context = ImGui::CreateContext();
            ImGui::GetIO().DisplaySize = ImVec2(320.0f, 200.0f);
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }

        ~ImGuiContextGuard()
        {
            ImGui::DestroyContext(context);
        }

        ImGuiContext* context = nullptr;
    };

    class ScopedLogListener final
    {
    public:
        explicit ScopedLogListener(std::function<void(const NLS::Debug::LogData&)> callback)
            : m_listener(NLS::Debug::Logger::LogEvent += std::move(callback))
        {
        }

        ~ScopedLogListener()
        {
            NLS::Debug::Logger::LogEvent -= m_listener;
        }

        ScopedLogListener(const ScopedLogListener&) = delete;
        ScopedLogListener& operator=(const ScopedLogListener&) = delete;

    private:
        NLS::ListenerID m_listener = NLS::InvalidListenerID;
    };
}

TEST(UiDrawDataSnapshotTests, EmptyDrawDataProducesNonVisibleSnapshot)
{
    ImGuiContextGuard guard;

    ImGui::NewFrame();
    ImGui::Render();

    const auto snapshot = NLS::Render::UI::CaptureUiDrawDataSnapshot(ImGui::GetDrawData(), 42u);

    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->frameId, 42u);
    EXPECT_FALSE(snapshot->hasVisibleDraws);
    EXPECT_FALSE(snapshot->containsUnsupportedUserCallback);
    EXPECT_EQ(snapshot->totalVertexCount, 0u);
    EXPECT_EQ(snapshot->totalIndexCount, 0u);
}

TEST(UiDrawDataSnapshotTests, CapturedSnapshotSurvivesNextImGuiFrame)
{
    ImGuiContextGuard guard;

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
    ImGui::SetNextWindowSize(ImVec2(120.0f, 80.0f));
    ImGui::Begin("snapshot-source", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::Button("capture me");
    ImGui::End();
    ImGui::Render();

    const auto snapshot = NLS::Render::UI::CaptureUiDrawDataSnapshot(ImGui::GetDrawData(), 100u);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_TRUE(snapshot->hasVisibleDraws);
    ASSERT_GT(snapshot->totalVertexCount, 0u);
    ASSERT_GT(snapshot->totalIndexCount, 0u);
    ASSERT_FALSE(snapshot->drawLists.empty());

    const auto copiedVertexCount = snapshot->drawLists.front().vertices.size();
    const auto copiedIndexCount = snapshot->drawLists.front().indices.size();
    const auto copiedCommandCount = snapshot->drawLists.front().commands.size();

    ImGui::NewFrame();

    EXPECT_EQ(snapshot->frameId, 100u);
    EXPECT_EQ(snapshot->drawLists.front().vertices.size(), copiedVertexCount);
    EXPECT_EQ(snapshot->drawLists.front().indices.size(), copiedIndexCount);
    EXPECT_EQ(snapshot->drawLists.front().commands.size(), copiedCommandCount);
    EXPECT_GT(copiedCommandCount, 0u);
}

TEST(UiDrawDataSnapshotTests, CapturedSnapshotRecordsCopyDiagnostics)
{
    ImGuiContextGuard guard;

    ImGui::NewFrame();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->AddRectFilled(ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f), IM_COL32_WHITE);
    ImGui::Render();

    const auto snapshot = NLS::Render::UI::CaptureUiDrawDataSnapshot(ImGui::GetDrawData(), 13u);

    ASSERT_NE(snapshot, nullptr);
    EXPECT_GT(snapshot->copyDiagnostics.cpuTimeNanoseconds, 0u);
    EXPECT_GT(snapshot->copyDiagnostics.copiedVertexBytes, 0u);
    EXPECT_GT(snapshot->copyDiagnostics.copiedIndexBytes, 0u);
    EXPECT_GT(snapshot->copyDiagnostics.copiedCommandCount, 0u);
}

TEST(UiDrawDataSnapshotTests, CapturedVerticesExpandPackedImGuiColorToFloatChannels)
{
    ImGuiContextGuard guard;

    ImGui::NewFrame();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->AddRectFilled(
        ImVec2(1.0f, 1.0f),
        ImVec2(4.0f, 4.0f),
        IM_COL32(0x11, 0x22, 0x33, 0x44));
    ImGui::Render();

    const auto snapshot = NLS::Render::UI::CaptureUiDrawDataSnapshot(ImGui::GetDrawData(), 12u);

    ASSERT_NE(snapshot, nullptr);
    ASSERT_FALSE(snapshot->drawLists.empty());
    ASSERT_FALSE(snapshot->drawLists.front().vertices.empty());
    const auto& vertex = snapshot->drawLists.front().vertices.front();
    EXPECT_FLOAT_EQ(vertex.color[0], 0x11 / 255.0f);
    EXPECT_FLOAT_EQ(vertex.color[1], 0x22 / 255.0f);
    EXPECT_FLOAT_EQ(vertex.color[2], 0x33 / 255.0f);
    EXPECT_FLOAT_EQ(vertex.color[3], 0x44 / 255.0f);
}

TEST(UiDrawDataSnapshotTests, UnsupportedUserCallbackIsDetectedWithoutExecutingIt)
{
    ImGuiContextGuard guard;
    g_unsupportedCallbackExecuted = false;
    bool sawUnsupportedCallbackDiagnostic = false;
    const ScopedLogListener listener(
        [&sawUnsupportedCallbackDiagnostic](const NLS::Debug::LogData& log)
        {
            if (log.logLevel == NLS::Debug::ELogLevel::LOG_WARNING &&
                log.message.find("unsupported ImGui user callback") != std::string::npos)
            {
                sawUnsupportedCallbackDiagnostic = true;
            }
        });

    ImGui::NewFrame();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->AddCallback(UnsupportedDrawCallback, nullptr);
    drawList->AddRectFilled(ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f), IM_COL32_WHITE);
    ImGui::Render();

    const auto snapshot = NLS::Render::UI::CaptureUiDrawDataSnapshot(ImGui::GetDrawData(), 7u);

    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(snapshot->containsUnsupportedUserCallback);
    EXPECT_TRUE(sawUnsupportedCallbackDiagnostic)
        << "Unsupported callbacks must emit a diagnostic before the RHI thread skips them.";
    EXPECT_FALSE(g_unsupportedCallbackExecuted);
}

TEST(UiDrawDataSnapshotTests, EncodedUiTextureIdentityRoundTripsThroughSnapshot)
{
    ImGuiContextGuard guard;

    const NLS::Render::UI::UiTextureId textureId { 123u, 7u };
    const auto encodedTextureId = NLS::Render::UI::PackUiTextureIdForImGui(textureId);
    ASSERT_NE(encodedTextureId, 0u);

    ImGui::NewFrame();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->AddImage(
        reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(encodedTextureId)),
        ImVec2(1.0f, 1.0f),
        ImVec2(4.0f, 4.0f));
    ImGui::Render();

    const auto snapshot = NLS::Render::UI::CaptureUiDrawDataSnapshot(ImGui::GetDrawData(), 9u);

    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(snapshot->hasVisibleDraws);
    EXPECT_FALSE(snapshot->containsUnsupportedTextureId);
    ASSERT_FALSE(snapshot->drawLists.empty());
    ASSERT_FALSE(snapshot->drawLists.front().commands.empty());
    const auto& command = snapshot->drawLists.front().commands.front();
    EXPECT_FALSE(command.hasUnsupportedTextureId);
    EXPECT_EQ(command.textureId.value, textureId.value);
    EXPECT_EQ(command.textureId.generation, textureId.generation);
}

TEST(UiDrawDataSnapshotTests, LegacyNativeTextureIdentityIsRejected)
{
    ImGuiContextGuard guard;

    ImGui::NewFrame();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->AddImage(
        reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(0x1234u)),
        ImVec2(1.0f, 1.0f),
        ImVec2(4.0f, 4.0f));
    ImGui::Render();

    const auto snapshot = NLS::Render::UI::CaptureUiDrawDataSnapshot(ImGui::GetDrawData(), 10u);

    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(snapshot->containsUnsupportedTextureId);
    EXPECT_FALSE(snapshot->hasVisibleDraws);
    ASSERT_FALSE(snapshot->drawLists.empty());
    ASSERT_FALSE(snapshot->drawLists.front().commands.empty());
    EXPECT_TRUE(snapshot->drawLists.front().commands.front().hasUnsupportedTextureId);
}

TEST(UiDrawDataSnapshotTests, InvalidUiTextureIdentityDoesNotCollapseToFontAtlas)
{
    const NLS::Render::UI::UiTextureId invalidGenerationId { 456u, 0u };
    const NLS::Render::UI::UiTextureId invalidZeroValueId { 0u, 1u };

    const auto encodedTextureId = NLS::Render::UI::PackUiTextureIdForImGui(invalidGenerationId);
    const auto encodedZeroValueTextureId = NLS::Render::UI::PackUiTextureIdForImGui(invalidZeroValueId);

    EXPECT_NE(encodedTextureId, 0u)
        << "Only a real font-atlas texture ID may encode as zero.";
    EXPECT_FALSE(NLS::Render::UI::UnpackUiTextureIdFromImGui(encodedTextureId).has_value());
    EXPECT_NE(encodedZeroValueTextureId, 0u)
        << "Only the exact font-atlas sentinel may encode as zero.";
    EXPECT_FALSE(NLS::Render::UI::UnpackUiTextureIdFromImGui(encodedZeroValueTextureId).has_value());
}

TEST(UiDrawDataSnapshotTests, ZeroSizedFramebufferDoesNotProduceVisibleSnapshot)
{
    ImGuiContextGuard guard;

    ImGui::GetIO().DisplaySize = ImVec2(0.0f, 200.0f);
    ImGui::NewFrame();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->AddRectFilled(ImVec2(1.0f, 1.0f), ImVec2(4.0f, 4.0f), IM_COL32_WHITE);
    ImGui::Render();

    const auto snapshot = NLS::Render::UI::CaptureUiDrawDataSnapshot(ImGui::GetDrawData(), 11u);

    ASSERT_NE(snapshot, nullptr);
    EXPECT_FALSE(snapshot->hasVisibleDraws);
}
