#include <gtest/gtest.h>

#include "ImGui/imgui.h"
#include "UI/Plugins/DragDrop.h"

namespace
{
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

	void AdvanceImGuiFrame()
	{
		ImGui::NewFrame();
		ImGui::EndFrame();
	}
}

TEST(DragDropPluginTests, PeekUsesCachedPayloadWhileWrapperDragSourceRemainsActive)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ImGuiContextGuard contextGuard;
    const int payloadValue = 42;
    NLS::UI::SetCachedDragDropPayloadForTesting("EditorAsset", &payloadValue, sizeof(payloadValue), true);

    const NLS::UI::DragDropPayloadView payload = NLS::UI::PeekDragDropPayload("EditorAsset");

    ASSERT_NE(payload.data, nullptr);
    ASSERT_EQ(payload.dataSize, sizeof(payloadValue));
    EXPECT_FALSE(payload.delivered);
    EXPECT_EQ(*static_cast<const int*>(payload.data), payloadValue);

    NLS::UI::ClearCachedDragDropPayload();
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required for DragDrop cache-state tests.";
#endif
}

TEST(DragDropPluginTests, PeekIgnoresCachedPayloadAfterCacheFrameExpires)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ImGuiContextGuard contextGuard;
    const int payloadValue = 42;
    NLS::UI::SetCachedDragDropPayloadForTesting("EditorAsset", &payloadValue, sizeof(payloadValue), true);
    AdvanceImGuiFrame();
    AdvanceImGuiFrame();

    const NLS::UI::DragDropPayloadView payload = NLS::UI::PeekDragDropPayload("EditorAsset");

    EXPECT_EQ(payload.data, nullptr);
    EXPECT_EQ(payload.dataSize, 0u);
    EXPECT_FALSE(payload.delivered);

    NLS::UI::ClearCachedDragDropPayload();
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required for DragDrop cache-state tests.";
#endif
}

TEST(DragDropPluginTests, PeekIgnoresStaleCachedPayloadSeededByTests)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ImGuiContextGuard contextGuard;
    const int payloadValue = 42;
    NLS::UI::SetCachedDragDropPayloadForTesting("EditorAsset", &payloadValue, sizeof(payloadValue), false);

    const NLS::UI::DragDropPayloadView payload = NLS::UI::PeekDragDropPayload("EditorAsset");

    EXPECT_EQ(payload.data, nullptr);
    EXPECT_EQ(payload.dataSize, 0u);
    EXPECT_FALSE(payload.delivered);

    NLS::UI::ClearCachedDragDropPayload();
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required for DragDrop cache-state tests.";
#endif
}
