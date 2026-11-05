#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <future>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>
#include <imgui.h>
#include <imgui_internal.h>
#include <UI/Modules/Canvas.h>
#include <UI/Fonts/FontGlyphFallback.h>
#include <UI/Icons/FontAwesomeIconFont.h>
#include <UI/Widgets/Selection/CheckBox.h>
#include <Scripting/ScriptErrorConsole.h>
#include "Core/EditorTaskProgressService.h"
#include "Core/EditorImportTaskProgress.h"
#include "Panels/Console.h"
#include "Panels/EditorStatusBar.h"
#include "Panels/BackgroundTasks.h"
#include <Input/InputSystem.h>
#include "Shortcuts/EditorShortcutService.h"
#include <filesystem>

namespace
{
using namespace NLS::Editor;

TEST(EditorTaskProgressTests, KeepsKnownProgressMonotonicAndTerminalResultsImmutable)
{
    Core::EditorTaskProgressService service;
    auto task = service.BeginTask("Import", "Queued", 0.0f);
    EXPECT_TRUE(task.Update("Parse", .7f));
    EXPECT_TRUE(task.Update("Late event", .2f));
    EXPECT_FLOAT_EQ(*service.GetSnapshot().front().progress, .7f);
    EXPECT_TRUE(task.Finish(Core::EditorTaskState::Failed, "Parse failed"));
    EXPECT_FALSE(task.IsRunning());
    EXPECT_FALSE(task.Update("Stale completion", 1.0f));
    EXPECT_FALSE(task.Finish(Core::EditorTaskState::Succeeded));
    const auto result = service.GetSnapshot().front();
    EXPECT_EQ(result.state, Core::EditorTaskState::Failed);
    EXPECT_EQ(result.detail, "Parse failed");
}

TEST(EditorTaskProgressTests, UnknownProgressAndInvalidFractionsAreNotFakePercentages)
{
    Core::EditorTaskProgressService service;
    auto task = service.BeginTask("Compile");
    EXPECT_FALSE(service.GetSnapshot().front().progress);
    task.Update("Unknown", std::numeric_limits<float>::quiet_NaN());
    EXPECT_FALSE(service.GetSnapshot().front().progress);
    task.Update("Known", 1.5f);
    EXPECT_FLOAT_EQ(*service.GetSnapshot().front().progress, 1);
    EXPECT_FALSE(task.Finish(Core::EditorTaskState::Running));
    task.Finish(Core::EditorTaskState::Succeeded);
}

TEST(EditorTaskProgressTests, CancellationRunsOutsideLockAndAwaitsProducerAcknowledgement)
{
    Core::EditorTaskProgressService service;
    int requests = 0;
    auto task = service.BeginTask("Import", {}, {}, [&]
    {
        ++requests;
        EXPECT_TRUE(service.GetSnapshot().front().cancellationRequested);
    });
    EXPECT_TRUE(service.RequestCancel(task.Id()));
    EXPECT_FALSE(service.RequestCancel(task.Id()));
    EXPECT_EQ(requests, 1);
    EXPECT_EQ(service.GetSnapshot().front().state, Core::EditorTaskState::Running);
    EXPECT_TRUE(task.IsRunning());
    task.Finish(Core::EditorTaskState::Cancelled, "Import cancelled");
    EXPECT_EQ(service.GetSnapshot().front().state, Core::EditorTaskState::Cancelled);
}

TEST(EditorTaskProgressTests, UnsupportedCancellationAndThrowingHandlersHaveExplicitResults)
{
    Core::EditorTaskProgressService service;
    auto task = service.BeginTask("Compile");
    EXPECT_FALSE(service.RequestCancel(task.Id()));
    auto failing = service.BeginTask("Import", {}, {}, [] { throw std::runtime_error("Cancel failed"); });
    EXPECT_TRUE(service.RequestCancel(failing.Id()));
    const auto snapshot = service.GetSnapshot();
    EXPECT_EQ(snapshot.back().state, Core::EditorTaskState::Failed);
    EXPECT_EQ(snapshot.back().detail, "Cancel failed");
    task.Finish(Core::EditorTaskState::Succeeded);
}

TEST(EditorTaskProgressTests, LastReportingHandleRecordsAbandonedWork)
{
    Core::EditorTaskProgressService service;
    try
    {
        auto task = service.BeginTask("Compile");
        auto retained = task;
        task = {};
        EXPECT_EQ(service.GetSnapshot().front().state, Core::EditorTaskState::Running);
        throw std::runtime_error("Worker failed");
    }
    catch (const std::exception&) {}
    EXPECT_EQ(service.GetSnapshot().front().state, Core::EditorTaskState::Failed);
}

TEST(EditorTaskProgressTests, HandlesSafelyOutliveTheServiceOwner)
{
    Core::EditorTaskHandle task;
    {
        Core::EditorTaskProgressService service;
        task = service.BeginTask("Retiring preview");
    }
    EXPECT_TRUE(task.Update("Worker returned"));
    EXPECT_TRUE(task.Finish(Core::EditorTaskState::Succeeded));
}

TEST(EditorTaskProgressTests, BoundsHistoryWithoutEvictingActiveTasksOrReusingIds)
{
    Core::EditorTaskProgressService service;
    auto active = service.BeginTask("Long task");
    Core::EditorTaskId previous = active.Id();
    for (int index = 0; index < 48; ++index)
    {
        auto task = service.BeginTask("Short task");
        EXPECT_GT(task.Id(), previous); previous = task.Id();
        task.Finish(Core::EditorTaskState::Succeeded);
    }
    const auto snapshot = service.GetSnapshot();
    ASSERT_EQ(snapshot.size(), 33u);
    EXPECT_EQ(snapshot.front().id, active.Id());
    EXPECT_EQ(snapshot[1].id, previous);
    service.ClearCompleted();
    ASSERT_EQ(service.GetSnapshot().size(), 1u);
    active.Finish(Core::EditorTaskState::Succeeded);
}

TEST(EditorTaskProgressTests, ConcurrentWorkersAndSnapshotsPreserveTaskResults)
{
    Core::EditorTaskProgressService service;
    std::atomic<int> finished = 0;
    std::vector<std::thread> workers;
    for (int index = 0; index < 4; ++index)
        workers.emplace_back([&]
        {
            for (int taskIndex = 0; taskIndex < 8; ++taskIndex)
            {
                auto task = service.BeginTask("Worker", {}, 0.0f);
                for (int phase = 0; phase < 8; ++phase) task.Update("Compiling", phase / 8.0f);
                task.Finish(Core::EditorTaskState::Succeeded);
            }
            ++finished;
        });
    while (finished != 4)
        for (const auto& task : service.GetSnapshot())
            if (task.progress) EXPECT_TRUE(*task.progress >= 0 && *task.progress <= 1);
    for (auto& worker : workers) worker.join();
    ASSERT_EQ(service.GetSnapshot().size(), 32u);
    for (const auto& task : service.GetSnapshot()) EXPECT_EQ(task.state, Core::EditorTaskState::Succeeded);
}

TEST(EditorImportTaskTests, KeepsLocalProgressSeparateFromTargetBatchAndPropagatesCancellation)
{
    Core::EditorTaskProgressService service;
    Assets::ImportProgressTracker imports;
    Core::EditorImportTaskProgress bridge(service, imports);
    const auto job = imports.BeginJob({}, "Assets/model.gltf", "win64-dx12", 2);
    imports.ReportProgress(job, Assets::ImportPhase::SourceParse, .6, "Parsing");
    const auto event = imports.GetCurrentEvent(job);
    ASSERT_TRUE(event);
    EXPECT_DOUBLE_EQ(event->localProgress, .6);
    EXPECT_DOUBLE_EQ(event->normalizedProgress, .3);
    auto snapshot = service.GetSnapshot();
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_FLOAT_EQ(*snapshot.front().progress, .6f);
    EXPECT_NE(snapshot.front().detail.find("batch 30%"), std::string::npos);
    EXPECT_TRUE(service.RequestCancel(snapshot.front().id));
    ASSERT_TRUE(imports.GetCancellationToken(job));
    EXPECT_TRUE(imports.GetCancellationToken(job)->get().IsCancellationRequested());
    imports.FinishJob(job, Assets::ImportJobTerminalStatus::Cancelled, {});
    EXPECT_EQ(service.GetSnapshot().front().state, Core::EditorTaskState::Cancelled);
}

TEST(EditorImportTaskTests, ConcurrentImportsKeepDistinctRecordsAndRealFailureStates)
{
    Core::EditorTaskProgressService service;
    Assets::ImportProgressTracker imports;
    Core::EditorImportTaskProgress bridge(service, imports);
    const auto first = imports.BeginJob({}, "Startup Scene", "scene", 2);
    const auto second = imports.BeginJob({}, "Assets/texture.png", "editor", 2);
    auto snapshot = service.GetSnapshot();
    ASSERT_EQ(snapshot.size(), 2u);
    EXPECT_FALSE(snapshot.front().cancellable);
    EXPECT_NE(snapshot.front().id, snapshot.back().id);
    imports.FinishJob(first, Assets::ImportJobTerminalStatus::Failed, {});
    EXPECT_EQ(service.GetSnapshot().back().state, Core::EditorTaskState::Failed);
    imports.FinishJob(second, Assets::ImportJobTerminalStatus::Succeeded, {});
    EXPECT_EQ(service.GetSnapshot().front().state, Core::EditorTaskState::Succeeded);
}

TEST(EditorImportTaskTests, RetiringBridgeStopsCallbacksWithoutDanglingCancellation)
{
    Core::EditorTaskProgressService service;
    Assets::ImportProgressTracker imports;
    auto bridge = std::make_unique<Core::EditorImportTaskProgress>(service, imports);
    const auto job = imports.BeginJob({}, "Assets/model.gltf", "editor", 1);
    const auto id = service.GetSnapshot().front().id;
    bridge.reset();
    EXPECT_EQ(service.GetSnapshot().front().state, Core::EditorTaskState::Failed);
    EXPECT_FALSE(service.RequestCancel(id));
    imports.ReportProgress(job, Assets::ImportPhase::ArtifactWrite, .8, "Writing");
    imports.FinishJob(job, Assets::ImportJobTerminalStatus::Succeeded, {});
    imports.BeginJob({}, "Assets/new.gltf", "editor", 1);
    EXPECT_EQ(service.GetSnapshot().size(), 1u);
    EXPECT_FALSE(service.RequestCancel(id));
}

TEST(EditorImportTaskTests, LateProgressAfterCompletionDoesNotResurrectAnImport)
{
    Core::EditorTaskProgressService service;
    Assets::ImportProgressTracker imports;
    std::promise<void> entered, release;
    auto observed = entered.get_future();
    auto resume = release.get_future().share();
    imports.Subscribe([&](const auto& event)
    {
        if (event.phase == Assets::ImportPhase::SourceParse)
        {
            entered.set_value();
            resume.wait();
        }
    });
    Core::EditorImportTaskProgress bridge(service, imports);
    const auto job = imports.BeginJob({}, "Assets/model.gltf", "editor", 1);
    std::thread worker([&] {imports.ReportProgress(job, Assets::ImportPhase::SourceParse, .5, "Parsing");});
    const auto status = observed.wait_for(std::chrono::seconds(2));
    imports.FinishJob(job, Assets::ImportJobTerminalStatus::Succeeded, {});
    release.set_value(); worker.join();
    ASSERT_EQ(status, std::future_status::ready);
    const auto snapshot = service.GetSnapshot();
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_EQ(snapshot.front().state, Core::EditorTaskState::Succeeded);
}

TEST(EditorImportTaskTests, CancellationRequestedByTheExistingImporterIsVisible)
{
    Core::EditorTaskProgressService service;
    Assets::ImportProgressTracker imports;
    Core::EditorImportTaskProgress bridge(service, imports);
    const auto job = imports.BeginJob({}, "Assets/model.gltf", "editor", 1);
    imports.GetCancellationToken(job)->get().Cancel();
    imports.ReportProgress(job, Assets::ImportPhase::ArtifactWrite, .5, "Writing");
    const auto snapshot = service.GetSnapshot();
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_TRUE(snapshot.front().cancellationRequested);
    EXPECT_EQ(snapshot.front().state, Core::EditorTaskState::Running);
    imports.FinishJob(job, Assets::ImportJobTerminalStatus::Cancelled, {});
}

class EditorStatusBarTests : public testing::Test
{
protected:
    double m_inputTime = 0;
    NLS::Input::InputSystem m_input{[this] { return m_inputTime; }};
    Shortcuts::EditorShortcutService m_shortcuts;
    bool m_consoleShortcuts = false;

    void BindConsoleShortcuts(Panels::Console& console)
    {
        m_shortcuts.InitializeInput(m_input);
        const auto defaults = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
            "App/Assets/Editor/Input/EditorShortcuts.inputactions";
        std::vector<std::string> diagnostics;
        ASSERT_TRUE(m_shortcuts.LoadDefaults(defaults, diagnostics));
        m_shortcuts.RegisterContext({"console", "Console", 50, "focused-panel",
            [&console] { return console.HasCurrentKeyboardFocus(); }});
        for (const std::string id : {"copy", "open"})
        {
            Shortcuts::ShortcutCommand command;
            command.id = "console." + id;
            command.context = "console";
            command.invoke = [&console, id](const Shortcuts::InvocationContext&)
            {
                return console.ExecuteCommand(id) ? Shortcuts::CommandResult::Completed : Shortcuts::CommandResult::Disabled;
            };
            ASSERT_TRUE(m_shortcuts.RegisterCommand(std::move(command)));
        }
        m_consoleShortcuts = true;
    }

    void Key(NLS::Windowing::Inputs::EKey key, ImGuiKey imguiKey, bool down, uint32_t modifiers = 0)
    {
        NLS::Input::InputEvent event;
        event.device = m_input.GetKeyboard();
        event.control = static_cast<uint16_t>(key);
        event.modifiers = modifiers;
        event.type = down ? NLS::Input::InputEventType::ButtonDown : NLS::Input::InputEventType::ButtonUp;
        ASSERT_TRUE(m_input.SubmitEvent(event));
        ImGui::GetIO().AddKeyEvent(imguiKey, down);
    }

    uint32_t PrimaryMask() const
    {
        return Shortcuts::GetPrimaryModifierForCurrentPlatform() == Shortcuts::EShortcutModifier::Super ? 8u : 2u;
    }

    void Primary(bool down)
    {
        const bool super = PrimaryMask() == 8u;
        Key(super ? NLS::Windowing::Inputs::EKey::KEY_LEFT_SUPER : NLS::Windowing::Inputs::EKey::KEY_LEFT_CONTROL,
            super ? ImGuiKey_LeftSuper : ImGuiKey_LeftCtrl, down, down ? PrimaryMask() : 0u);
        ImGui::GetIO().AddKeyEvent(super ? ImGuiMod_Super : ImGuiMod_Ctrl, down);
    }

    void SetUp() override
    {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1000, 600); io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    void TearDown() override { ImGui::DestroyContext(); }
    template<class Draw> void Frame(Draw draw, ImVec2 mouse = ImVec2(-100,-100), bool down = false)
    {
        auto& io = ImGui::GetIO(); io.MousePos = mouse; io.MouseDown[0] = down;
        ImGui::NewFrame(); draw();
        if (m_consoleShortcuts)
        {
            Shortcuts::EditorInputRoute route;
            route.textEditing = io.WantTextInput;
            m_shortcuts.UpdateInput(route);
        }
        ImGui::Render();
        m_input.EndFrame();
        m_inputTime += io.DeltaTime;
    }
    template<class Draw> void Click(Draw draw, ImVec2 position)
    {
        Frame(draw, position); Frame(draw, position, true); Frame(draw, position, false);
    }
};

TEST_F(EditorStatusBarTests, FirstOpenHasUsableSizeWithoutOverridingExplicitResize)
{
    NLS::UI::PanelWindowSettings settings;
    Panels::Console console("Console", true, settings);
    Frame([&] { console.Draw(); });
    auto* window = ImGui::FindWindowByName("Console##Console");
    ASSERT_NE(window, nullptr);
    EXPECT_GE(window->Size.x, 600.0f);
    EXPECT_GE(window->Size.y, 300.0f);

    Frame([&]
    {
        ImGui::SetNextWindowSize(ImVec2(260, 180), ImGuiCond_Always);
        console.Draw();
    });
    EXPECT_FLOAT_EQ(window->Size.x, 260.0f);
    EXPECT_FLOAT_EQ(window->Size.y, 180.0f);
    Frame([&] { console.Draw(); });
    EXPECT_FLOAT_EQ(window->Size.x, 260.0f);
    EXPECT_FLOAT_EQ(window->Size.y, 180.0f);
}

TEST_F(EditorStatusBarTests, SystemFallbackRendersChineseAndKeepsRequestedGlyphsOnRebuild)
{
#ifdef _WIN32
    const std::filesystem::path fontPath("C:/Windows/Fonts/msyh.ttc");
#elif defined(__APPLE__)
    const std::filesystem::path fontPath("/System/Library/Fonts/PingFang.ttc");
#else
    const std::filesystem::path fontPath("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
#endif
    if (!std::filesystem::exists(fontPath)) GTEST_SKIP() << "No system CJK font available";
    auto& atlas = *ImGui::GetIO().Fonts;
    atlas.Clear();
    NLS::UI::FontGlyphFallback fallback(atlas, fontPath.string());
    auto* font = atlas.AddFontDefault();
    const std::vector<ImWchar> icons = { NLS::UI::Icons::kFontAwesomeSearchGlyph,
        NLS::UI::Icons::kFontAwesomeTimesGlyph };
    ASSERT_TRUE(NLS::UI::Icons::EnsureFontAwesomeIconFontLoaded(14.0f, font, icons));
    ASSERT_TRUE(fallback.Merge(atlas, font, 14.0f));
    ASSERT_TRUE(atlas.Build());
    EXPECT_NE(font->FindGlyphNoFallback(0x5f53), nullptr); // The first character in the captured CA1834 diagnostic.
    auto* secondFont = atlas.AddFontDefault();
    ASSERT_TRUE(NLS::UI::Icons::EnsureFontAwesomeIconFontLoaded(16.0f, secondFont, icons));
    ASSERT_TRUE(fallback.Merge(atlas, secondFont, 16.0f));
    ASSERT_TRUE(atlas.Build());
    EXPECT_NE(font->FindGlyphNoFallback(NLS::UI::Icons::kFontAwesomeSearchGlyph), nullptr);
    EXPECT_NE(font->FindGlyphNoFallback(0x5f53), nullptr);

    fallback.RequestText("\xe9\xbe\x98", font); // A less common CJK character, U+9F98.
    ASSERT_TRUE(fallback.IsDirty());
    atlas.Clear();
    fallback.PrepareRanges();
    font = atlas.AddFontDefault();
    ASSERT_TRUE(NLS::UI::Icons::EnsureFontAwesomeIconFontLoaded(14.0f, font, icons));
    ASSERT_TRUE(fallback.Merge(atlas, font, 14.0f));
    ASSERT_TRUE(atlas.Build());
    EXPECT_FALSE(fallback.IsDirty());
    EXPECT_NE(font->FindGlyphNoFallback(0x5f53), nullptr);
    EXPECT_NE(font->FindGlyphNoFallback(0x9f98), nullptr);
    EXPECT_NE(font->FindGlyphNoFallback(NLS::UI::Icons::kFontAwesomeSearchGlyph), nullptr);
}

TEST_F(EditorStatusBarTests, HiddenConsoleUpdatesLatestLogOnUiThreadAndClearsOnPlay)
{
    Panels::Console console("Console", false, {});
    NLS::Debug::LogData data; data.message = "Background warning"; data.logLevel = NLS::Debug::ELogLevel::LOG_WARNING;
    std::thread worker([&] { console.OnLogIntercepted(data); }); worker.join();
    EXPECT_FALSE(console.GetStatusSnapshot().hasMessage);
    console.UpdateLogState();
    EXPECT_FALSE(console.IsOpened());
    EXPECT_EQ(console.GetStatusSnapshot().message, data.message);
    EXPECT_EQ(console.GetStatusSnapshot().level, NLS::Debug::ELogLevel::LOG_WARNING);
    console.ClearOnPlay();
    EXPECT_FALSE(console.GetStatusSnapshot().hasMessage);
}

TEST_F(EditorStatusBarTests, LatestMessageDoesNotPreferErrorsOrRespectConsoleFilters)
{
    Panels::Console console("Console", false, {});
    auto options = console.GetOptions();
    options.errors = false;
    console.SetOptions(options);
    NLS::Debug::LogData data; data.message = "Hidden error"; data.logLevel = NLS::Debug::ELogLevel::LOG_ERROR;
    console.OnLogIntercepted(data); console.UpdateLogState();
    EXPECT_EQ(console.GetStatusSnapshot().message, "Hidden error");
    data.message = "Latest information"; data.logLevel = NLS::Debug::ELogLevel::LOG_INFO;
    console.OnLogIntercepted(data); console.UpdateLogState();
    EXPECT_EQ(console.GetStatusSnapshot().message, "Latest information");
    EXPECT_EQ(console.GetStatusSnapshot().level, NLS::Debug::ELogLevel::LOG_INFO);
    console.Clear(); EXPECT_FALSE(console.GetStatusSnapshot().hasMessage);
}

TEST_F(EditorStatusBarTests, HiddenConsoleUsesScriptSeverityAndRequestsErrorPause)
{
    Panels::Console console("Console", false, {});
    auto options = console.GetOptions();
    options.errorPause = true;
    options.errors = false;
    console.SetOptions(options);
    NLS::Scripting::ScriptErrorConsole errors;
    console.SetScriptConsole(&errors);
    NLS::Scripting::ScriptError diagnostic;
    diagnostic.language = NLS::Scripting::ScriptLanguage::Lua;
    diagnostic.severity = NLS::Scripting::ScriptErrorSeverity::Warning;
    diagnostic.message = "warning";
    errors.Push(diagnostic);
    console.UpdateLogState();
    EXPECT_EQ(console.GetStatusSnapshot().level, NLS::Debug::ELogLevel::LOG_WARNING);
    EXPECT_FALSE(console.ConsumeErrorPauseRequest());
    diagnostic.severity = NLS::Scripting::ScriptErrorSeverity::Error;
    diagnostic.message = "filtered error";
    errors.Push(diagnostic);
    console.UpdateLogState();
    EXPECT_FALSE(console.IsOpened());
    EXPECT_TRUE(console.ConsumeErrorPauseRequest());
    EXPECT_FALSE(console.ConsumeErrorPauseRequest());
    EXPECT_EQ(console.GetLogModel().Counts().warnings, 1u);
    EXPECT_EQ(console.GetLogModel().Counts().errors, 1u);
}

TEST_F(EditorStatusBarTests, ConsoleToolbarFiltersAndSearchUseMouseAndKeyboard)
{
    Panels::Console console("Console", true, {});
    int optionsChanged = 0;
    console.SetOptionsChanged([&](const auto&) { ++optionsChanged; });
    NLS::Debug::LogData log;
    log.logLevel = NLS::Debug::ELogLevel::LOG_INFO;
    log.message = "Needle in a message";
    console.OnLogIntercepted(log);
    log.logLevel = NLS::Debug::ELogLevel::LOG_WARNING;
    log.message = "unrelated warning";
    console.OnLogIntercepted(log);
    console.UpdateLogState();
    auto draw = [&]
    {
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_Always);
        console.Draw();
    };
    Frame(draw); Frame(draw);
    auto* window = ImGui::FindWindowByName("Console##Console");
    ASSERT_NE(window, nullptr);
    const float y = window->Pos.y + window->TitleBarHeight + ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight() * .5f;
    const auto findControl = [&](ImGuiID id)
    {
        for (float x = window->Pos.x + 5; x < window->Pos.x + window->Size.x - 5; x += 2)
        {
            Frame(draw, ImVec2(x, y));
            if (ImGui::GetCurrentContext()->HoveredId == id) return ImVec2(x, y);
        }
        return ImVec2(-1, -1);
    };
    const auto warning = findControl(window->GetID("###console-warnings"));
    ASSERT_GT(warning.x, 0);
    Click(draw, warning);
    EXPECT_FALSE(console.GetOptions().warnings);
    EXPECT_EQ(optionsChanged, 1);
    EXPECT_EQ(console.GetLogModel().Counts().warnings, 1u);
    const auto search = findControl(window->GetID("##console-search"));
    ASSERT_GT(search.x, 0);
    Click(draw, search);
    ImGui::GetIO().AddInputCharactersUTF8("needle");
    Frame(draw, search);
    EXPECT_EQ(console.GetFilter().search, "needle");
    EXPECT_EQ(console.GetLogModel().BuildRows(console.GetFilter()).size(), 1u);
}

TEST_F(EditorStatusBarTests, SplitterPersistsOptionsOnceAfterDrag)
{
    Panels::Console console("Console", true, {});
    int optionsChanged = 0;
    float savedRatio = 0.0f;
    console.SetOptionsChanged([&](const auto& options) { ++optionsChanged; savedRatio = options.detailRatio; });
    auto draw = [&]
    {
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_Always);
        console.Draw();
    };
    Frame(draw); Frame(draw);
    ImGuiWindow* list = nullptr;
    for (auto* window : ImGui::GetCurrentContext()->Windows)
        if (std::string(window->Name).find("##console-list") != std::string::npos) { list = window; break; }
    ASSERT_NE(list, nullptr);
    const ImVec2 splitter(list->Pos.x + 50, list->Pos.y + list->Size.y + ImGui::GetStyle().ItemSpacing.y + 2);
    Frame(draw, splitter);
    Frame(draw, splitter, true);
    Frame(draw, ImVec2(splitter.x, splitter.y - 20), true);
    EXPECT_EQ(optionsChanged, 0);
    Frame(draw, ImVec2(splitter.x, splitter.y - 20), false);
    EXPECT_EQ(optionsChanged, 1);
    EXPECT_GT(savedRatio, .30f);
}

TEST_F(EditorStatusBarTests, NarrowConsoleKeepsOverflowAccessibleAtHighDpi)
{
    ImGui::GetIO().FontGlobalScale = 2.0f;
    Panels::Console console("Console", true, {});
    auto draw = [&]
    {
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_Always);
        console.Draw();
    };
    Frame(draw); Frame(draw);
    auto* window = ImGui::FindWindowByName("Console##Console");
    ASSERT_NE(window, nullptr);
    const auto menuId = window->GetID("...##console-menu");
    const float y = window->Pos.y + window->TitleBarHeight + ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight() * .5f;
    ImVec2 menu(-1, -1);
    for (float x = window->Pos.x + 5; x < window->Pos.x + window->Size.x - 5; x += 2)
    {
        Frame(draw, ImVec2(x, y));
        if (ImGui::GetCurrentContext()->HoveredId == menuId) { menu = ImVec2(x, y); break; }
    }
    ASSERT_GT(menu.x, 0);
    Click(draw, menu);
    EXPECT_GT(ImGui::GetCurrentContext()->OpenPopupStack.Size, 0);
}

TEST_F(EditorStatusBarTests, CopyKeepsSelectionOnlyWhileCompilerRecordRemains)
{
    Panels::Console console("Console", true, {});
    BindConsoleShortcuts(console);
    Panels::ConsoleLogEntry diagnostic;
    diagnostic.message = "pinned compiler error";
    diagnostic.level = NLS::Debug::ELogLevel::LOG_ERROR;
    diagnostic.source = Panels::ConsoleEntrySource::Build;
    diagnostic.received = std::chrono::steady_clock::now();
    console.ReplaceCompilerDiagnostics({diagnostic});
    std::string clipboard;
    auto& io = ImGui::GetIO();
    io.ClipboardUserData = &clipboard;
    io.SetClipboardTextFn = [](void* user, const char* text) { *static_cast<std::string*>(user) = text; };
    auto draw = [&]
    {
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_Always);
        console.Draw();
    };
    Frame(draw); Frame(draw);
    ImGuiWindow* list = nullptr;
    for (auto* window : ImGui::GetCurrentContext()->Windows)
        if (std::string(window->Name).find("##console-list") != std::string::npos) { list = window; break; }
    ASSERT_NE(list, nullptr);
    const ImVec2 row(list->Pos.x + 40, list->Pos.y + 15);
    Click(draw, row);
    const auto copy = [&]
    {
        Primary(true);
        Key(NLS::Windowing::Inputs::EKey::KEY_C, ImGuiKey_C, true, PrimaryMask());
        Frame(draw, row);
        Key(NLS::Windowing::Inputs::EKey::KEY_C, ImGuiKey_C, false, PrimaryMask());
        Primary(false);
        Frame(draw, row);
    };
    copy();
    ASSERT_NE(clipboard.find("pinned compiler error"), std::string::npos);
    NLS::Debug::LogData ordinary; ordinary.message = "ordinary message";
    console.OnLogIntercepted(ordinary); console.UpdateLogState();
    console.Clear();
    clipboard.clear();
    copy();
    EXPECT_NE(clipboard.find("pinned compiler error"), std::string::npos);
    console.ReplaceCompilerDiagnostics({});
    clipboard.clear();
    copy();
    EXPECT_TRUE(clipboard.empty());
}

TEST_F(EditorStatusBarTests, LargeCountsLeaveSeverityButtonsAndMenuInsideToolbar)
{
    ImGui::GetIO().FontGlobalScale = 1.5f;
    Panels::Console console("Console", true, {});
    NLS::Debug::LogData data;
    data.message = "Repeated message";
    for (int i = 0; i < 10000; ++i) console.OnLogIntercepted(data);
    console.UpdateLogState();
    auto draw = [&]
    {
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(800, 400), ImGuiCond_Always);
        console.Draw();
    };
    Frame(draw); Frame(draw);
    auto* window = ImGui::FindWindowByName("Console##Console");
    ASSERT_NE(window, nullptr);
    const auto errorId = window->GetID("###console-errors");
    const auto menuId = window->GetID("...##console-menu");
    const float y = window->Pos.y + window->TitleBarHeight + ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight() * .5f;
    bool foundError = false, foundMenu = false;
    for (float x = window->Pos.x + 5; x < window->Pos.x + window->Size.x - 5; x += 2)
    {
        Frame(draw, ImVec2(x, y));
        const auto hovered = ImGui::GetCurrentContext()->HoveredId;
        foundError |= hovered == errorId;
        foundMenu |= hovered == menuId;
    }
    EXPECT_TRUE(foundError);
    EXPECT_TRUE(foundMenu);
}

TEST_F(EditorStatusBarTests, ScrollbarMovementStopsFollowingNewLogs)
{
    Panels::Console console("Console", true, {});
    NLS::Debug::LogData data;
    for (int i = 0; i < 100; ++i)
    {
        data.message = "message " + std::to_string(i);
        console.OnLogIntercepted(data);
    }
    console.UpdateLogState();
    auto draw = [&]
    {
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_Always);
        console.Draw();
    };
    Frame(draw); Frame(draw); Frame(draw);
    ImGuiWindow* list = nullptr;
    for (auto* window : ImGui::GetCurrentContext()->Windows)
        if (std::string(window->Name).find("##console-list") != std::string::npos) { list = window; break; }
    ASSERT_NE(list, nullptr);
    ASSERT_GT(list->ScrollMax.y, 0);
    ASSERT_NEAR(list->Scroll.y, list->ScrollMax.y, 1.0f);
    data.message = "new message while following";
    console.OnLogIntercepted(data); console.UpdateLogState();
    Frame(draw); Frame(draw);
    ASSERT_NEAR(list->Scroll.y, list->ScrollMax.y, 1.0f);
    const auto scrollbar = ImGui::GetWindowScrollbarRect(list, ImGuiAxis_Y);
    const ImVec2 track((scrollbar.Min.x + scrollbar.Max.x) * .5f, scrollbar.Min.y + 5);
    Click(draw, track);
    Frame(draw, track);
    const float readingPosition = list->Scroll.y;
    ASSERT_LT(readingPosition, list->ScrollMax.y - 1.0f);
    data.message = "new message while reading";
    console.OnLogIntercepted(data); console.UpdateLogState();
    Frame(draw, track); Frame(draw, track);
    EXPECT_NEAR(list->Scroll.y, readingPosition, 1.0f);
}

TEST_F(EditorStatusBarTests, ScriptDiagnosticsContinueAfterBoundedBufferRollsAndClears)
{
    Panels::Console console("Console", false, {});
    NLS::Scripting::ScriptErrorConsole errors(2);
    NLS::Scripting::ScriptError error; error.message = "First";
    errors.Push(error); console.UpdateLogState(&errors);
    error.message = "Second"; errors.Push(error);
    error.message = "Third"; errors.Push(error); console.UpdateLogState(&errors);
    EXPECT_NE(console.GetStatusSnapshot().message.find("Third"), std::string::npos);
    const auto sequence = console.GetStatusSnapshot().sequence;
    error.message = "Fourth"; errors.Push(error); console.UpdateLogState(&errors);
    EXPECT_EQ(console.GetStatusSnapshot().sequence, sequence + 1);
    EXPECT_NE(console.GetStatusSnapshot().message.find("Fourth"), std::string::npos);
    const auto sourceSequence = errors.GetNextSequence();
    errors.Clear(); console.Clear();
    error.message = "After clear"; errors.Push(error); console.UpdateLogState(&errors);
    EXPECT_GT(errors.GetNextSequence(), sourceSequence);
    EXPECT_NE(console.GetStatusSnapshot().message.find("After clear"), std::string::npos);
}

TEST_F(EditorStatusBarTests, GeneralAndScriptMessagesUseReceiptOrderInsteadOfFlushOrder)
{
    Panels::Console console("Console", false, {});
    NLS::Scripting::ScriptErrorConsole errors;
    NLS::Scripting::ScriptError error; error.message = "Earlier script error";
    errors.Push(error);
    NLS::Debug::LogData data; data.message = "Later general log"; data.logLevel = NLS::Debug::ELogLevel::LOG_INFO;
    console.OnLogIntercepted(data);
    console.UpdateLogState(&errors);
    EXPECT_EQ(console.GetStatusSnapshot().message, "Later general log");
    console.OnLogIntercepted(data);
    error.message = "Latest script error"; errors.Push(error);
    console.UpdateLogState(&errors);
    EXPECT_NE(console.GetStatusSnapshot().message.find("Latest script error"), std::string::npos);
}

TEST_F(EditorStatusBarTests, ConfiguredScriptSourceClearsAndOpensDiagnostics)
{
    Panels::Console console("Console", false, {});
    NLS::Scripting::ScriptErrorConsole errors;
    int opened = 0;
    console.SetScriptConsole(&errors, [&](const auto& error)
    {
        ++opened;
        EXPECT_EQ(error.message, "Open this diagnostic");
    });
    NLS::Scripting::ScriptError error; error.message = "Open this diagnostic";
    error.sourcePath = "Assets/Example.cs"; error.line = 8;
    errors.Push(error); console.UpdateLogState();
    console.Open();
    auto draw = [&]
    {
        ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_Always);
        console.Draw();
    };
    Frame(draw); Frame(draw);
    ImGuiWindow* list = nullptr;
    for (auto* window : ImGui::GetCurrentContext()->Windows)
        if (std::string(window->Name).find("##console-list") != std::string::npos) { list = window; break; }
    ASSERT_NE(list, nullptr);
    const ImVec2 row(list->Pos.x + 30.0f, list->Pos.y + 15.0f);
    Click(draw, row); Click(draw, row);
    EXPECT_EQ(opened, 1);
    console.Clear();
    EXPECT_TRUE(errors.GetErrors().empty());
    console.UpdateLogState();
    EXPECT_FALSE(console.GetStatusSnapshot().hasMessage);
}

TEST_F(EditorStatusBarTests, OpeningFallsBackToTheFirstAvailableStackLocation)
{
    Panels::Console console("Console", true, {});
    BindConsoleShortcuts(console);
    int opened = 0;
    console.SetSourceAvailability([](const std::string& path) { return path == "Assets/Valid.lua"; });
    console.SetScriptConsole(nullptr, [&](const auto& error)
    {
        ++opened;
        EXPECT_EQ(error.sourcePath, "Assets/Valid.lua");
        EXPECT_EQ(error.line, 37);
    });
    NLS::Debug::LogData data;
    data.message = "source fallback";
    data.sourceFile = "Assets/Missing.cs";
    data.scriptStack = "at Run() in Assets/Missing.cs:line 4\nat Update() in Assets/Valid.lua:line 37";
    console.OnLogIntercepted(data); console.UpdateLogState();
    auto draw = [&]
    {
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_Always);
        console.Draw();
    };
    Frame(draw); Frame(draw);
    ImGuiWindow* list = nullptr;
    for (auto* window : ImGui::GetCurrentContext()->Windows)
        if (std::string(window->Name).find("##console-list") != std::string::npos) { list = window; break; }
    ASSERT_NE(list, nullptr);
    const ImVec2 row(list->Pos.x + 40, list->Pos.y + 15);
    Click(draw, row); Click(draw, row);
    EXPECT_EQ(opened, 1);
    Key(NLS::Windowing::Inputs::EKey::KEY_ENTER, ImGuiKey_Enter, true); Frame(draw, row);
    Key(NLS::Windowing::Inputs::EKey::KEY_ENTER, ImGuiKey_Enter, false); Frame(draw, row);
    EXPECT_EQ(opened, 2);
}

TEST_F(EditorStatusBarTests, StatusBarMouseRoutesOpenConsoleAndTaskPanelAndPanelReopens)
{
    Panels::Console console("Console", false, {});
    Core::EditorTaskProgressService service;
    NLS::UI::PanelWindowSettings settings; settings.closable = true;
    Panels::BackgroundTasks tasks("Background Tasks", false, settings, service);
    Panels::EditorStatusBar bar;
    int consoleClicks = 0, taskClicks = 0;
    bar.SetSources(console, service, [&] { ++consoleClicks; }, [&] { ++taskClicks; tasks.Open(); }, [] { return 60.0f; });
    auto draw = [&] { tasks.Draw(); bar.Draw(); };
    Frame(draw); Frame(draw);
    Click(draw, ImVec2(40, 588));
    EXPECT_EQ(consoleClicks, 1);
    auto* barWindow = ImGui::FindWindowByName(("EditorStatusBar" + bar.GetPanelID()).c_str());
    ASSERT_NE(barWindow, nullptr);
    const auto taskId = barWindow->GetID("##backgroundTasks");
    ImVec2 taskPoint(-1, 588);
    for (float x = 20; x < 1000; x += 10)
    {
        Frame(draw, ImVec2(x, 588));
        if (ImGui::GetCurrentContext()->HoveredId == taskId) {taskPoint.x = x;break;}
    }
    ASSERT_GT(taskPoint.x, 0);
    Click(draw, taskPoint);
    EXPECT_EQ(taskClicks, 1); EXPECT_TRUE(tasks.IsOpened());
    Frame(draw);
    auto* taskWindow = ImGui::FindWindowByName((tasks.name + tasks.GetPanelID()).c_str());
    ASSERT_NE(taskWindow, nullptr);
    const auto closePoint = ImVec2(taskWindow->Pos.x + taskWindow->Size.x - ImGui::GetStyle().FramePadding.x - ImGui::GetFontSize() * .5f,
        taskWindow->Pos.y + ImGui::GetStyle().FramePadding.y + ImGui::GetFontSize() * .5f);
    Click(draw, closePoint);
    EXPECT_FALSE(tasks.IsOpened());
    Click(draw, taskPoint);
    EXPECT_EQ(taskClicks, 2); EXPECT_TRUE(tasks.IsOpened());
    Frame([&] { tasks.Draw(); bar.Draw(); });
}

TEST_F(EditorStatusBarTests, DockspaceReservesStatusBarHeightAfterLayoutChanges)
{
    NLS::UI::Canvas canvas; Panels::EditorStatusBar bar;
    canvas.AddPanel(bar); canvas.MakeDockspace(true);
    Frame([&] { canvas.Draw(); });
    auto* dock = ImGui::FindWindowByName("##dockspace"); ASSERT_NE(dock, nullptr);
    auto* status = ImGui::FindWindowByName(("EditorStatusBar" + bar.GetPanelID()).c_str()); ASSERT_NE(status, nullptr);
    EXPECT_FLOAT_EQ(status->Size.y, 24);
    EXPECT_FLOAT_EQ(dock->Size.y, 576);
    bar.SetHeight(48);
    Frame([&] { canvas.Draw(); });
    EXPECT_FLOAT_EQ(dock->Size.y, 552);
    bar.SetHeight(24);
    Frame([&] { canvas.Draw(); });
    EXPECT_FLOAT_EQ(dock->Size.y, 576);
}

TEST_F(EditorStatusBarTests, TaskPanelCancelWaitsForProducerAndClearRemovesTheResult)
{
    Core::EditorTaskProgressService service;
    int requests = 0;
    auto task = service.BeginTask("Import model", "Parsing source", .4f, [&] {++requests;});
    Panels::BackgroundTasks panel("Background Tasks", true, {}, service);
    auto draw = [&]
    {
        ImGui::SetNextWindowPos(ImVec2(80,80), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(500,360), ImGuiCond_Always);
        panel.Draw();
    };
    Frame(draw); Frame(draw);
    auto* window = ImGui::FindWindowByName((panel.name + panel.GetPanelID()).c_str());
    ASSERT_NE(window, nullptr);
    auto locate = [&](ImGuiID id)
    {
        ImVec2 point(-1,-1);
        for (float y = window->Pos.y + 25; y < window->Pos.y + window->Size.y - 8; y += 5)
        {
            Frame(draw, ImVec2(window->Pos.x + 20, y));
            if (ImGui::GetCurrentContext()->HoveredId == id) {point = ImVec2(window->Pos.x + 20, y);break;}
        }
        return point;
    };
    const auto taskSeed = ImHashStr(std::to_string(task.Id()).c_str(), 0, window->ID);
    const auto cancelPoint = locate(ImHashStr("Cancel", 0, taskSeed));
    ASSERT_GT(cancelPoint.x, 0);
    Click(draw, cancelPoint);
    EXPECT_EQ(requests, 1);
    EXPECT_TRUE(service.GetSnapshot().front().cancellationRequested);
    EXPECT_TRUE(task.IsRunning());
    Frame(draw);
    task.Finish(Core::EditorTaskState::Cancelled, "Importer acknowledged cancellation");
    EXPECT_EQ(service.GetSnapshot().front().state, Core::EditorTaskState::Cancelled);
    const auto clearPoint = locate(ImHashStr("Clear completed", 0, window->ID));
    ASSERT_GT(clearPoint.x, 0);
    Click(draw, clearPoint);
    EXPECT_TRUE(service.GetSnapshot().empty());
}

TEST_F(EditorStatusBarTests, LongUtf8MessagesStaySingleLineAndWithinAvailableWidth)
{
    const std::string text = "Long UTF-8 \xe4\xb8\xad\xe6\x96\x87 message\nwith\ttabs\rand 100% formatting";
    Frame([&]
    {
        const auto line = Panels::EditorStatusBar::SingleLine(text);
        EXPECT_EQ(line.find_first_of("\r\n\t"), std::string::npos);
        for (float width = 1; width < 300; width += 3)
        {
            const auto label = Panels::EditorStatusBar::Elide(line, width);
            EXPECT_LE(ImGui::CalcTextSize(label.c_str()).x, width);
            if (label.ends_with("...") && label.size() > 3)
            {
                const auto cut = label.size() - 3;
                EXPECT_NE(static_cast<unsigned char>(line[cut]) & 0xc0, 0x80);
            }
        }
    });
}
}
