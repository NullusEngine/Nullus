#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    std::filesystem::path RepoPath(std::string_view relativePath)
    {
        return std::filesystem::path(NLS_ROOT_DIR) / std::filesystem::path(relativePath);
    }

    std::string ReadSourceText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        EXPECT_TRUE(input.is_open()) << "Failed to open source file: " << path.string();
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    std::string ExtractFunctionBody(const std::string& source, std::string_view functionNeedle)
    {
        const auto begin = source.find(functionNeedle);
        EXPECT_NE(begin, std::string::npos) << "Missing function: " << functionNeedle;
        if (begin == std::string::npos)
            return {};

        const auto bodyBegin = source.find('{', begin);
        EXPECT_NE(bodyBegin, std::string::npos) << "Missing function body: " << functionNeedle;
        if (bodyBegin == std::string::npos)
            return {};

        size_t depth = 0u;
        for (size_t offset = bodyBegin; offset < source.size(); ++offset)
        {
            if (source[offset] == '{')
            {
                ++depth;
            }
            else if (source[offset] == '}')
            {
                --depth;
                if (depth == 0u)
                    return source.substr(bodyBegin, offset - bodyBegin + 1u);
            }
        }

        ADD_FAILURE() << "Unterminated function body: " << functionNeedle;
        return {};
    }

    void ExpectNoNeedles(
        const std::string& source,
        const std::vector<std::string_view>& needles,
        std::string_view context)
    {
        for (const auto needle : needles)
        {
            EXPECT_EQ(source.find(needle), std::string::npos)
                << context << " must not contain " << needle;
        }
    }
}

TEST(RHIUiOverlaySourceGuardTests, UIManagerRenderDoesNotCallLegacyBridgeDrawData)
{
    const auto source = ReadSourceText(RepoPath("Runtime/UI/UIManager.cpp"));
    const auto renderBody = ExtractFunctionBody(source, "void UIManager::Render(");

    const auto publishBranch = renderBody.find("ShouldPublishUiSnapshotToFrameGraph()");
    const auto publishCall = renderBody.find("PublishCurrentUiSnapshotToFrameGraph()", publishBranch);

    ASSERT_NE(publishBranch, std::string::npos);
    ASSERT_NE(publishCall, std::string::npos);
    EXPECT_EQ(renderBody.find("m_uiBridge->RenderDrawData"), std::string::npos)
        << "DX12 legacy UI direct-submit rendering must not remain reachable from UIManager::Render.";
    EXPECT_EQ(renderBody.find("ImGui_ImplDX12_RenderDrawData"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, UIManagerBeginFrameAllowsNullRendererBridgeOnFrameGraphOverlayPath)
{
    const auto source = ReadSourceText(RepoPath("Runtime/UI/UIManager.cpp"));
    const auto beginFrameBody = ExtractFunctionBody(source, "void UIManager::BeginFrame(");

    const auto bridgeBackendCheck = beginFrameBody.find("!m_uiBridge->HasRendererBackend()");
    const auto overlayCapabilityCheck = beginFrameBody.find("!ShouldPublishUiSnapshotToFrameGraph()", bridgeBackendCheck);
    ASSERT_NE(bridgeBackendCheck, std::string::npos);
    ASSERT_NE(overlayCapabilityCheck, std::string::npos)
        << "A null/unsupported legacy renderer bridge must not prevent ImGui::NewFrame when "
           "UIOverlayFrameGraph owns migrated rendering.";
}

TEST(RHIUiOverlaySourceGuardTests, UIManagerDoesNotWarnForNullBridgeWhenFrameGraphOverlayOwnsRendering)
{
    const auto source = ReadSourceText(RepoPath("Runtime/UI/UIManager.cpp"));
    const auto constructorBody = ExtractFunctionBody(source, "UIManager::UIManager(");

    const auto officialBackendWarning = constructorBody.find("runtime initialization did not complete");
    ASSERT_NE(officialBackendWarning, std::string::npos);

    const auto overlayCapabilityCheck = constructorBody.rfind(
        "ShouldPublishUiSnapshotToFrameGraph()",
        officialBackendWarning);
    ASSERT_NE(overlayCapabilityCheck, std::string::npos)
        << "Migrated UIOverlayFrameGraph rendering intentionally uses the null bridge, so the legacy "
           "official-backend warning must be gated by frame-graph ownership.";
}

TEST(RHIUiOverlaySourceGuardTests, EditorAndLauncherDoNotSubmitLegacyUiRendering)
{
    const auto editorSource = ReadSourceText(RepoPath("Project/Editor/Core/Editor.cpp"));
    const auto launcherSource = ReadSourceText(RepoPath("Project/Launcher/Core/Launcher.cpp"));
    const auto renderEditorUiBody = ExtractFunctionBody(
        editorSource,
        "void Editor::Core::Editor::RenderEditorUI(");

    EXPECT_EQ(renderEditorUiBody.find("ResolveUISignalSemaphore()"), std::string::npos);
    EXPECT_EQ(renderEditorUiBody.find("SetUICompositionSignal"), std::string::npos);
    EXPECT_EQ(renderEditorUiBody.find("SubmitUIRendering()"), std::string::npos);

    const auto launcherRunBody = ExtractFunctionBody(launcherSource, "LauncherRunResult Launcher::Run(");
    EXPECT_EQ(launcherRunBody.find("SubmitUIRendering()"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, OverlayRendererFontAtlasAndTextureRegistryDoNotOwnNativeDx12QueueWork)
{
    const std::vector<std::filesystem::path> migratedFiles = {
        RepoPath("Runtime/Rendering/UI/RHIImGuiOverlayRenderer.cpp"),
        RepoPath("Runtime/Rendering/UI/RHIImGuiFontAtlas.cpp"),
        RepoPath("Runtime/Rendering/UI/RHIImGuiTextureRegistry.cpp"),
    };

    for (const auto& path : migratedFiles)
    {
        ASSERT_TRUE(std::filesystem::exists(path)) << "Missing migrated UI file: " << path.string();
        const auto source = ReadSourceText(path);
        ExpectNoNeedles(
            source,
            {
                "ExecuteCommandLists(",
                "->ExecuteCommandLists",
                "Signal(",
                "->Signal(",
                "PresentInternal",
                "->Present(",
                ".Present(",
                "ImGui_ImplDX12_RenderDrawData",
            },
            path.generic_string());
    }
}

TEST(RHIUiOverlaySourceGuardTests, FontAtlasUploadAvoidsImmediateCreateTextureInitialData)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/UI/RHIImGuiFontAtlas.cpp"));

    EXPECT_EQ(source.find("CreateTexture(textureDesc, uploadDesc)"), std::string::npos)
        << "Font atlas upload must not use immediate RHIDevice::CreateTexture(initialData) because the "
           "DX12 implementation owns a private command list/fence wait on that path.";
    EXPECT_NE(source.find("CreateTexture(textureDesc)"), std::string::npos);
    EXPECT_NE(source.find("CopyBufferToTexture"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, OverlayRendererDoesNotExposeRecordConveniencePath)
{
    const auto header = ReadSourceText(RepoPath("Runtime/Rendering/UI/RHIImGuiOverlayRenderer.h"));

    EXPECT_EQ(header.find("Record("), std::string::npos)
        << "UI overlay recording must be split into PrepareFrameResources before the render pass "
           "and RecordPrepared inside the render pass; public Record(...) convenience paths can bypass "
           "dynamic-buffer preparation or record upload barriers inside an active render pass.";
}

TEST(RHIUiOverlaySourceGuardTests, DX12LegacyBridgeImplementationIsRemoved)
{
    const auto path = RepoPath("Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp");
    if (!std::filesystem::exists(path))
        return;

    const auto source = ReadSourceText(path);
    ExpectNoNeedles(
        source,
        {
            "ImGui_ImplDX12_RenderDrawData",
            "WaitForBackbufferReuse",
            "WaitForAllocatorReuse",
            "DX12UIBridge::ExecuteCommandLists",
            "ID3D12CommandQueue",
            "SubmitCommandBuffer",
            "CreateDX12RHIUIBridge",
        },
        path.generic_string());
}

TEST(RHIUiOverlaySourceGuardTests, RHIUIBridgeFactoryNeverCreatesLegacyDX12Bridge)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp"));
    const auto factoryBody = ExtractFunctionBody(source, "std::unique_ptr<RHIUIBridge> CreateRHIUIBridge(");

    const auto capabilityCheck = factoryBody.find("GetUIOverlayFrameGraphFeature");
    const auto nullBridgeReturn = factoryBody.find("return std::make_unique<NullUIBridge>()", capabilityCheck);

    ASSERT_NE(capabilityCheck, std::string::npos)
        << "The UI bridge factory must inspect UIOverlayFrameGraph capability before selecting a legacy bridge.";
    ASSERT_NE(nullBridgeReturn, std::string::npos)
        << "When UIOverlayFrameGraph is supported, the factory must choose the null bridge and let "
           "the RHI overlay renderer own UI work.";
    EXPECT_EQ(factoryBody.find("CreateDX12RHIUIBridge"), std::string::npos)
        << "The legacy DX12 UI bridge must not remain as a runtime fallback.";
    EXPECT_NE(factoryBody.find("overlayFeature.reason"), std::string::npos)
        << "Unsupported UIOverlayFrameGraph state must log the capability reason instead of silently "
           "choosing a legacy renderer bridge.";
}

TEST(RHIUiOverlaySourceGuardTests, RhiThreadPrepareUiRenderDoesNotStartStandaloneFrameWhenOverlayIsSupported)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/RhiThreadCoordinator.cpp"));
    const auto prepareBody = ExtractFunctionBody(source, "bool RhiThreadCoordinator::PrepareUIRender(");
    const auto presentBody = ExtractFunctionBody(source, "void RhiThreadCoordinator::PresentSwapchain(");

    const auto capabilityCheck = prepareBody.find("GetUIOverlayFrameGraphFeature");
    const auto standaloneBegin = prepareBody.find("BeginStandaloneUiExplicitFrame");
    ASSERT_NE(standaloneBegin, std::string::npos);
    ASSERT_NE(capabilityCheck, std::string::npos)
        << "PrepareUIRender must inspect UIOverlayFrameGraph before considering the legacy "
           "standalone UI explicit frame path.";
    EXPECT_LT(capabilityCheck, standaloneBegin)
        << "Migrated UI overlay devices must fail closed before standalone UI explicit frame startup.";
    EXPECT_EQ(prepareBody.find("PublishUiOnlyFrame"), std::string::npos)
        << "PrepareUIRender must not consume the pending UI snapshot before a scene package can attach it.";
    EXPECT_NE(presentBody.find("PublishUiOnlyFrame"), std::string::npos)
        << "PresentSwapchain owns the UI-only fallback after scene publication had a chance to consume the snapshot.";
    const auto drainSceneWork = presentBody.find("DrainPendingSceneOrUiOverlayFrame");
    const auto publishUiOnly = presentBody.find("PublishPendingUiOnlyFrame");
    ASSERT_NE(drainSceneWork, std::string::npos);
    ASSERT_NE(publishUiOnly, std::string::npos);
    EXPECT_LT(drainSceneWork, publishUiOnly)
        << "PresentSwapchain must drain already-published scene builders before using the UI-only fallback.";
}

TEST(RHIUiOverlaySourceGuardTests, UIManagerResolveTextureViewUsesPackedUiTextureIdentityOnMigratedPath)
{
    const auto source = ReadSourceText(RepoPath("Runtime/UI/UIManager.cpp"));
    const auto resolveBody = ExtractFunctionBody(
        source,
        "NLS::Render::RHI::NativeHandle UIManager::ResolveTextureView(");

    ASSERT_NE(resolveBody.find("ShouldPublishUiSnapshotToFrameGraph()"), std::string::npos);
    EXPECT_NE(resolveBody.find("DriverUIAccess::RegisterUiTextureView"), std::string::npos);
    EXPECT_NE(resolveBody.find("PackUiTextureIdForImGui"), std::string::npos);
    EXPECT_NE(resolveBody.find("nativeHandle.value"), std::string::npos);
    EXPECT_NE(resolveBody.find("nativeHandle.handle"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, RegisteredUiTextureContractDocumentsPreviousFrameStaticScope)
{
    const auto contract = ReadSourceText(
        RepoPath("specs/049-integrate-ui-framegraph/contracts/rhi-ui-overlay-pass-contract.md"));

    EXPECT_NE(contract.find("previous-frame/static sampled resources"), std::string::npos);
    EXPECT_NE(contract.find("does not infer same-frame producer dependencies"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, ImageWidgetsPreferPackedUiTextureIdentityWhenAvailable)
{
    const auto imageSource = ReadSourceText(RepoPath("Runtime/UI/Widgets/Visual/Image.cpp"));
    const auto imageBody = ExtractFunctionBody(imageSource, "void Image::_Draw_Impl()");
    ASSERT_NE(imageBody.find("ResolveTextureId(textureView)"), std::string::npos);
    EXPECT_EQ(imageBody.find("nativeHandle."), std::string::npos);

    const auto buttonSource = ReadSourceText(RepoPath("Runtime/UI/Widgets/Buttons/ButtonImage.cpp"));
    const auto buttonBody = ExtractFunctionBody(buttonSource, "void ButtonImage::_Draw_Impl()");
    ASSERT_NE(buttonBody.find("ResolveTextureId(textureView)"), std::string::npos);
    EXPECT_EQ(buttonBody.find("nativeHandle."), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, DirectImageCallSitesUseUnifiedTextureIdResolver)
{
    const auto editorTopBarSource = ReadSourceText(RepoPath("Project/Editor/Panels/EditorTopBar.cpp"));
    const auto editorResolveBody = ExtractFunctionBody(editorTopBarSource, "void* ResolveTextureId(");
    EXPECT_NE(editorResolveBody.find("ResolveTextureId(p_textureView)"), std::string::npos);
    EXPECT_EQ(editorResolveBody.find("IsValid()"), std::string::npos);

    const auto launcherSource = ReadSourceText(RepoPath("Project/Launcher/Core/Launcher.cpp"));
    const auto drawTextureBody = ExtractFunctionBody(launcherSource, "void DrawTexture(");
    EXPECT_NE(drawTextureBody.find("ResolveTextureId(textureView)"), std::string::npos);
    EXPECT_EQ(drawTextureBody.find("IsValid()"), std::string::npos);

    const auto assetBrowserSource = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto assetBrowserResolveBody = ExtractFunctionBody(assetBrowserSource, "void* ResolveAssetBrowserTextureHandle(");
    EXPECT_NE(assetBrowserResolveBody.find("ResolveTextureId(textureView)"), std::string::npos);
    EXPECT_EQ(assetBrowserResolveBody.find("ResolveTextureView(textureView)"), std::string::npos);
    EXPECT_EQ(assetBrowserResolveBody.find("IsValid()"), std::string::npos);

    const auto thumbnailResolveBody = ExtractFunctionBody(
        assetBrowserSource,
        "Editor::Panels::AssetBrowser::ThumbnailTextureHandle Editor::Panels::AssetBrowser::ResolveCachedThumbnailTextureHandle(");
    EXPECT_NE(
        thumbnailResolveBody.find("ResolveTextureId(found->second.textureView)"),
        std::string::npos);
    EXPECT_EQ(thumbnailResolveBody.find("ResolveTextureView(found->second.textureView)"), std::string::npos);
    EXPECT_EQ(thumbnailResolveBody.find("IsValid()"), std::string::npos);

    const auto thumbnailLoadBody = ExtractFunctionBody(
        assetBrowserSource,
        "bool Editor::Panels::AssetBrowser::LoadDecodedCachedThumbnailTexture(");
    EXPECT_NE(thumbnailLoadBody.find("ResolveTextureId(textureView) == nullptr"), std::string::npos);
    EXPECT_EQ(thumbnailLoadBody.find("ResolveTextureView(textureView)"), std::string::npos);
    EXPECT_EQ(thumbnailLoadBody.find("IsValid()"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, UIManagerResourceNotificationsRouteThroughFrameGraphOverlayResources)
{
    const auto source = ReadSourceText(RepoPath("Runtime/UI/UIManager.cpp"));

    const auto notifyResizeBody = ExtractFunctionBody(source, "void UIManager::NotifySwapchainWillResize()");
    EXPECT_NE(notifyResizeBody.find("ShouldPublishUiSnapshotToFrameGraph()"), std::string::npos);
    EXPECT_NE(notifyResizeBody.find("DriverUIAccess::NotifyUiOverlaySwapchainWillResize"), std::string::npos);

    const auto releaseTextureBody = ExtractFunctionBody(source, "void UIManager::ReleaseTextureViewHandle(");
    EXPECT_NE(releaseTextureBody.find("ShouldPublishUiSnapshotToFrameGraph()"), std::string::npos);
    EXPECT_NE(releaseTextureBody.find("DriverUIAccess::ReleaseUiTextureView"), std::string::npos);

    const auto rebuildFontsBody = ExtractFunctionBody(source, "void UIManager::RebuildFonts()");
    EXPECT_NE(rebuildFontsBody.find("NotifyFontAtlasChanged()"), std::string::npos);

    const auto notifyFontsBody = ExtractFunctionBody(source, "void UIManager::NotifyFontAtlasChanged()");
    EXPECT_NE(notifyFontsBody.find("ShouldPublishUiSnapshotToFrameGraph()"), std::string::npos);
    EXPECT_NE(notifyFontsBody.find("DriverUIAccess::NotifyUiOverlayFontAtlasChanged"), std::string::npos);

    const auto loadFontBody = ExtractFunctionBody(source, "bool UIManager::LoadFont(");
    EXPECT_NE(loadFontBody.find("NotifyFontAtlasChanged()"), std::string::npos);

    const auto unloadFontBody = ExtractFunctionBody(source, "bool UIManager::UnloadFont(");
    EXPECT_NE(unloadFontBody.find("RebuildFonts()"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, DriverUIAccessExposesUiOverlayResourceLifecycleHooks)
{
    const auto header = ReadSourceText(RepoPath("Runtime/Rendering/Context/DriverAccess.h"));
    EXPECT_NE(header.find("NotifyUiOverlayFontAtlasChanged"), std::string::npos);
    EXPECT_NE(header.find("NotifyUiOverlaySwapchainWillResize"), std::string::npos);

    const auto driverSource = ReadSourceText(RepoPath("Runtime/Rendering/Context/Driver.cpp"));
    const auto fontBody = ExtractFunctionBody(
        driverSource,
        "void DriverUIAccess::NotifyUiOverlayFontAtlasChanged(");
    EXPECT_NE(fontBody.find("uiOverlayRenderer"), std::string::npos);
    EXPECT_NE(fontBody.find("InvalidateFontAtlas"), std::string::npos);

    const auto resizeBody = ExtractFunctionBody(
        driverSource,
        "void DriverUIAccess::NotifyUiOverlaySwapchainWillResize(");
    EXPECT_EQ(resizeBody.find("ReleaseRetiredResources"), std::string::npos);
    EXPECT_EQ(resizeBody.find("ReleaseRetiredTextureViews"), std::string::npos);
}
