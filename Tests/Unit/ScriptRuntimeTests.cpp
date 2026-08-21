#include "Scripting/ScriptComponent.h"
#include "Scripting/ScriptRuntime.h"
#include "Scripting/ScriptApiManifest.h"
#include "Scripting/ScriptFieldSerialization.h"
#include "Scripting/SerializedObject.h"
#include "Scripting/ScriptErrorConsole.h"
#include "Scripting/FakeScriptBackend.h"
#include "Scripting/LuaScriptBackend.h"
#include "Scripting/CoreClrScriptBackend.h"
#include "Scripting/ScriptScheduler.h"
#include "Scripting/ScriptRuntimeSetup.h"
#include "Scripting/ScriptDebug.h"
#include "Debug/ExternalCodeEditor.h"
#include "Debug/ProjectDebugWorkspace.h"
#include "Debug/VSCodeConfigurationGenerator.h"
#include "Debug/VisualStudioConfigurationGenerator.h"
#include "Scripting/ManagedScriptBuildService.h"
#include "Engine/GameObject.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/LightComponent.h"
#include "Engine/Components/TransformComponent.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/PrefabDocument.h"
#include "SceneSystem/Scene.h"

#include <gtest/gtest.h>
#include <Json/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
using namespace NLS::Scripting;

class RecordingBackend final : public IScriptBackend
{
public:
    ScriptBackendId GetBackendId() const override { return {7}; }
    ScriptLanguage GetLanguage() const override { return ScriptLanguage::Lua; }
    ScriptBackendCapabilities GetCapabilities() const override { return {true, true, true, true}; }

    ScriptStatus Initialize(const ScriptApiDatabase&) override
    {
        initialized = true;
        return ScriptStatus::Ok();
    }

    void Shutdown() override { initialized = false; }

    ScriptStatus LoadScript(const ScriptAsset& asset) override
    {
        loadedSources.push_back(asset.sourceText);
        return ScriptStatus::Ok();
    }
    ScriptStatus UnloadScript(const NLS::Core::Assets::AssetId&) override { return ScriptStatus::Ok(); }

    ScriptStatus CreateInstance(
        const ScriptAsset&,
        NativeObjectHandle,
        ScriptInstanceHandle& output) override
    {
        output = {7, 1, nextInstanceIndex++};
        return ScriptStatus::Ok();
    }

    ScriptStatus DestroyInstance(ScriptInstanceHandle instance) override
    {
        destroyed.insert(instance);
        return ScriptStatus::Ok();
    }

    ScriptStatus Invoke(
        ScriptInstanceHandle instance,
        ScriptCallback callback,
        const ScriptInvocationContext&) override
    {
        invokedInstances.push_back(instance);
        events.push_back(ToString(callback));
        if (failingInstances.contains(instance))
            return ScriptStatus::Error(ScriptStatusCode::RuntimeError, "batch failure");
        return ScriptStatus::Ok();
    }

    ScriptStatus Reload(const NLS::Core::Assets::AssetId&, const ScriptApiDatabase&) override
    {
        return rejectReload
            ? ScriptStatus::Error(ScriptStatusCode::HotReloadRejected, "test reload rejection")
            : ScriptStatus::Ok();
    }

    bool GetField(ScriptInstanceHandle, ScriptFieldId field, ScriptValue& output) override
    {
        const auto found = fields.find(field);
        if (found == fields.end())
            return false;
        output = found->second;
        return true;
    }

    bool SetField(ScriptInstanceHandle, ScriptFieldId field, const ScriptValue& value) override
    {
        fields[field] = value;
        setFields.push_back(field);
        return true;
    }

    bool initialized = false;
    std::vector<std::string> events;
    std::unordered_set<ScriptInstanceHandle> destroyed;
    std::unordered_map<ScriptFieldId, ScriptValue> fields;
    std::vector<ScriptFieldId> setFields;
    std::vector<std::string> loadedSources;
    std::vector<ScriptInstanceHandle> invokedInstances;
    std::unordered_set<ScriptInstanceHandle> failingInstances;
    uint32_t nextInstanceIndex = 1;
    bool rejectReload = false;
};

class DiagnosticBackend final : public IScriptBackend, public IScriptDiagnosticProvider
{
public:
    ScriptBackendId GetBackendId() const override { return {8}; }
    ScriptLanguage GetLanguage() const override { return ScriptLanguage::Lua; }
    ScriptBackendCapabilities GetCapabilities() const override { return {}; }
    ScriptStatus Initialize(const ScriptApiDatabase&) override { return ScriptStatus::Ok(); }
    void Shutdown() override {}
    ScriptStatus LoadScript(const ScriptAsset&) override { return ScriptStatus::Ok(); }
    ScriptStatus UnloadScript(const NLS::Core::Assets::AssetId&) override { return ScriptStatus::Ok(); }
    ScriptStatus CreateInstance(const ScriptAsset&, NativeObjectHandle, ScriptInstanceHandle& output) override
    {
        output = {8, 1, 1};
        return ScriptStatus::Ok();
    }
    ScriptStatus DestroyInstance(ScriptInstanceHandle) override { return ScriptStatus::Ok(); }
    ScriptStatus Invoke(ScriptInstanceHandle instance, ScriptCallback, const ScriptInvocationContext&) override
    {
        m_diagnostic = ScriptError{};
        m_diagnostic->instance = instance;
        m_diagnostic->message = "structured Lua failure";
        m_diagnostic->stackTrace = "LuaStack: Update -> Move";
        m_diagnostic->line = 17;
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, "fallback status text");
    }
    ScriptStatus Reload(const NLS::Core::Assets::AssetId&, const ScriptApiDatabase&) override { return ScriptStatus::Ok(); }
    bool GetField(ScriptInstanceHandle, ScriptFieldId, ScriptValue&) override { return false; }
    bool SetField(ScriptInstanceHandle, ScriptFieldId, const ScriptValue&) override { return false; }
    std::optional<ScriptError> ConsumeLastDiagnostic() override
    {
        auto diagnostic = std::move(m_diagnostic);
        m_diagnostic.reset();
        return diagnostic;
    }

private:
    std::optional<ScriptError> m_diagnostic;
};

std::filesystem::path FindNullusRoot()
{
#ifdef NLS_ROOT_DIR
    const auto configuredRoot = std::filesystem::path(NLS_ROOT_DIR);
    if (std::filesystem::is_regular_file(
            configuredRoot / "Managed" / "Nullus.GameScripts" / "Nullus.GameScripts.csproj"))
    {
        return configuredRoot;
    }
#endif

    auto candidate = std::filesystem::current_path();
    while (!candidate.empty())
    {
        if (std::filesystem::is_regular_file(
                candidate / "Managed" / "Nullus.GameScripts" / "Nullus.GameScripts.csproj"))
            return candidate;
        const auto parent = candidate.parent_path();
        if (parent == candidate)
            break;
        candidate = parent;
    }
    return {};
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::filesystem::path ResolveManagedOutput(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> candidates;
#ifdef NLS_BUILD_DIR
    #ifdef NLS_BUILD_CONFIGURATION
    candidates.push_back(std::filesystem::path(NLS_BUILD_DIR) / "Managed" / NLS_BUILD_CONFIGURATION);
    #endif
    candidates.push_back(std::filesystem::path(NLS_BUILD_DIR) / "Managed" / "Debug");
    candidates.push_back(std::filesystem::path(NLS_BUILD_DIR) / "Managed" / "Release");
#endif

    candidates.insert(candidates.end(), {
        root / "Library" / "ScriptBuild" / "Managed",
        root / "build-scripting-vs5" / "Managed" / "Debug",
        root / "Managed" / "Nullus.GameScripts" / "bin" / "Debug" / "net8.0",
        root / "Managed" / "Nullus.GameScripts" / "bin" / "Release" / "net8.0"});
    for (const auto& candidate : candidates)
    {
        if (std::filesystem::is_regular_file(candidate / "GameScripts.dll")
            && std::filesystem::is_regular_file(candidate / "GameScripts.runtimeconfig.json"))
        {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path ResolveDotnetRoot(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> candidates;
    for (const char* name : {
        "NLS_DOTNET_ROOT",
        "DOTNET_ROOT_X64",
        "DOTNET_ROOT",
        "DOTNET_ROOT(x86)"})
    {
        if (const auto* value = std::getenv(name); value != nullptr && *value != '\0')
            candidates.emplace_back(value);
    }

#if defined(_WIN32)
    constexpr const char* platform = "windows";
    constexpr const char* architecture = "x64";
#elif defined(__APPLE__)
    constexpr const char* platform = "macos";
    constexpr const char* architecture = "x64";
#else
    constexpr const char* platform = "linux";
    constexpr const char* architecture = "x64";
#endif
    candidates.push_back(root / "Tools" / "Dotnet" / platform / architecture);

    for (const auto& candidate : candidates)
    {
        if (std::filesystem::is_directory(candidate / "host" / "fxr"))
            return candidate;
    }
    return {};
}
}

TEST(ScriptRuntimeTests, ScenePlayAndStopToggleRuntimeState)
{
    NLS::Engine::SceneSystem::Scene scene;
    EXPECT_FALSE(scene.IsPlaying());
    scene.Play();
    EXPECT_TRUE(scene.IsPlaying());
    scene.Stop();
    EXPECT_FALSE(scene.IsPlaying());
}

TEST(ScriptRuntimeTests, RoutesLifecycleThroughBackend)
{
    auto backend = std::make_unique<RecordingBackend>();
    auto* backendPtr = backend.get();

    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize({}).Succeeded());
    ASSERT_TRUE(backendPtr->initialized);

    NLS::Scripting::ScriptComponent component;
    component.SetRuntime(&runtime);
    ScriptAsset asset;
    asset.language = ScriptLanguage::Lua;
    asset.sourcePath = "Assets/Test.lua";
    asset.scriptType = 1;
    component.SetScriptAsset(std::move(asset));
    component.OnCreate();
    component.OnAwake();
    component.OnEnable();
    component.OnStart();
    component.OnUpdate(0.016f);
    component.OnFixedUpdate(0.02f);
    component.OnLateUpdate(0.016f);
    component.OnDisable();
    component.OnDestroy();
    component.OnDestroy();

    ASSERT_EQ(backendPtr->events.size(), 8u);
    EXPECT_EQ(backendPtr->events[0], "Awake");
    EXPECT_EQ(backendPtr->events[1], "OnEnable");
    EXPECT_EQ(backendPtr->events[2], "Start");
    EXPECT_EQ(backendPtr->events[3], "Update");
    EXPECT_EQ(backendPtr->events[4], "FixedUpdate");
    EXPECT_EQ(backendPtr->events[5], "LateUpdate");
    EXPECT_EQ(backendPtr->events[6], "OnDisable");
    EXPECT_EQ(backendPtr->events[7], "OnDestroy");
    EXPECT_EQ(backendPtr->destroyed.size(), 1u);
}

TEST(ScriptRuntimeTests, SceneScriptAssetResolutionFillsTransientSourceMetadata)
{
    ScriptComponent component;
    ScriptAsset serialized;
    serialized.assetId = NLS::Core::Assets::AssetId::New();
    serialized.language = ScriptLanguage::CSharp;
    serialized.scriptType = 17;
    component.SetScriptAsset(serialized);

    ScriptAsset imported = serialized;
    imported.sourcePath = "Assets/RotatingCube.cs";
    imported.sourceText = "public sealed class RotatingCube : Behaviour { }";
    imported.contentHash = 42;
    imported.isComponent = true;
    component.ResolveScriptAsset(imported);

    EXPECT_EQ(component.GetScriptAsset().assetId, serialized.assetId);
    EXPECT_EQ(component.GetScriptAsset().sourcePath, "Assets/RotatingCube.cs");
    EXPECT_EQ(component.GetScriptAsset().sourceText, imported.sourceText);
    EXPECT_EQ(component.GetScriptAsset().contentHash, 42u);
    EXPECT_TRUE(component.GetScriptAsset().isComponent);
}

TEST(ScriptRuntimeTests, ReportConsumesStructuredBackendDiagnostic)
{
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::make_unique<DiagnosticBackend>()).Succeeded());
    ASSERT_TRUE(runtime.Initialize({}).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    asset.sourcePath = "Assets/Diagnostics.lua";
    asset.sourceText = "return {}";
    ScriptInstanceHandle instance;
    ASSERT_TRUE(runtime.LoadScript(asset).Succeeded());
    ASSERT_TRUE(runtime.CreateInstance(asset, NativeObjectHandle::FromInstanceId(12), instance).Succeeded());

    const auto status = runtime.Invoke(instance, ScriptCallback::Update, {});
    EXPECT_EQ(status.code, ScriptStatusCode::RuntimeError);
    ASSERT_EQ(runtime.GetErrors().size(), 1u);
    const auto& error = runtime.GetErrors().front();
    EXPECT_EQ(error.language, ScriptLanguage::Lua);
    EXPECT_EQ(error.scriptAsset, asset.assetId);
    EXPECT_EQ(error.instance, instance);
    EXPECT_EQ(error.sourcePath, asset.sourcePath);
    EXPECT_EQ(error.line, 17);
    EXPECT_EQ(error.message, "structured Lua failure");
    EXPECT_EQ(error.stackTrace, "LuaStack: Update -> Move");
}

TEST(ScriptRuntimeTests, ScriptDebugServiceForwardsDiagnosticToEditorOpener)
{
    ScriptRuntime runtime;
    ScriptDebugService service(runtime);
    ScriptError diagnostic;
    diagnostic.language = ScriptLanguage::CSharp;
    diagnostic.sourcePath = "Assets/Player.cs";
    diagnostic.line = 23;
    diagnostic.column = 7;

    bool called = false;
    service.SetDiagnosticOpener([&](const ScriptError& error)
    {
        called = true;
        EXPECT_EQ(error.sourcePath, "Assets/Player.cs");
        EXPECT_EQ(error.line, 23);
        EXPECT_EQ(error.column, 7);
        return true;
    });

    EXPECT_TRUE(service.OpenDiagnostic(diagnostic));
    EXPECT_TRUE(called);
    diagnostic.sourcePath.clear();
    EXPECT_FALSE(service.OpenDiagnostic(diagnostic));
}

#if NLS_HAS_LUA_VM && NLS_HAS_LUAPANDA_SOCKET
TEST(ScriptRuntimeTests, ScriptDebugServiceAppliesLuaPandaSettingsToBackend)
{
    auto backend = std::make_unique<LuaScriptBackend>(ScriptBackendId{9});
    auto* backendPtr = backend.get();
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize({}).Succeeded());

    ScriptDebugService service(runtime);
    EXPECT_FALSE(backendPtr->IsLuaPandaDebuggingEnabled());
    EXPECT_FALSE(backendPtr->IsLuaPandaDebuggerActive());

    auto settings = service.GetSettings();
    settings.enableLuaPanda = true;
    service.SetSettings(settings);
    EXPECT_TRUE(backendPtr->IsLuaPandaDebuggingEnabled());
    EXPECT_TRUE(backendPtr->IsLuaPandaDebuggerActive());

    settings.enableLuaPanda = false;
    service.SetSettings(settings);
    EXPECT_FALSE(backendPtr->IsLuaPandaDebuggingEnabled());
    EXPECT_FALSE(backendPtr->IsLuaPandaDebuggerActive());
}
#endif

TEST(ScriptRuntimeTests, ExternalCodeEditorBuildsStableVisualStudioCodeLocation)
{
    const auto arguments = NLS::Editor::Debug::ExternalCodeEditor::BuildVisualStudioCodeArguments(
        "Assets/Player Script.cs",
        0,
        0);
    ASSERT_EQ(arguments.size(), 3u);
    EXPECT_EQ(arguments[0], "--reuse-window");
    EXPECT_EQ(arguments[1], "--goto");
    EXPECT_EQ(arguments[2], "Assets/Player Script.cs:1:1");
}

TEST(ScriptRuntimeTests, VSCodeConfigurationGenerationPreservesCppLaunchEntry)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("NullusScriptDebug-" + NLS::Core::Assets::AssetId::New().ToString());
    std::filesystem::create_directories(root / ".vscode");
    {
        std::ofstream output(root / ".vscode" / "launch.json", std::ios::binary);
        output << "// Keep the existing native launch profile.\n"
            << R"({"version":"0.2.0","configurations":[{"name":"Nullus: C++ Editor","type":"cppvsdbg","request":"launch","program":"${workspaceFolder}/Editor.exe"}]})";
    }

    const auto result = NLS::Editor::Debug::GenerateVSCodeConfiguration(root);
    ASSERT_TRUE(result.success) << result.errorMessage;

    std::ifstream launchInput(root / ".vscode" / "launch.json", std::ios::binary);
    const auto launch = nlohmann::json::parse(launchInput);
    ASSERT_TRUE(launch["configurations"].is_array());
    EXPECT_EQ(launch["configurations"].size(), 4u);
    EXPECT_EQ(launch["configurations"][0]["name"], "Nullus: C# Scripts");
    EXPECT_EQ(launch["configurations"][1]["name"], "Nullus: Lua Scripts");
    EXPECT_EQ(launch["configurations"][2]["name"], "Nullus: C# + Lua");
    EXPECT_EQ(launch["configurations"][3]["name"], "Nullus: C++ Editor");
    EXPECT_FALSE(launch["configurations"][1]["useCHook"].get<bool>());
    EXPECT_EQ(launch["configurations"][0]["processId"], "${command:nullus.resolveEditorProcess}");

    std::ifstream extensionsInput(root / ".vscode" / "extensions.json", std::ios::binary);
    const auto extensions = nlohmann::json::parse(extensionsInput);
    EXPECT_TRUE(std::find(
        extensions["recommendations"].begin(),
        extensions["recommendations"].end(),
        "stuartwang.luapanda@3.3.1") != extensions["recommendations"].end());
    EXPECT_TRUE(std::find(
        extensions["recommendations"].begin(),
        extensions["recommendations"].end(),
        "ms-dotnettools.csharp") != extensions["recommendations"].end());
    EXPECT_TRUE(std::find(
        extensions["recommendations"].begin(),
        extensions["recommendations"].end(),
        "nullus.nullus-script-debugger") != extensions["recommendations"].end());
    launchInput.close();
    extensionsInput.close();
    std::filesystem::remove_all(root);
}

TEST(ScriptRuntimeTests, VSCodeConfigurationGenerationUsesLuaDebugSettings)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("NullusScriptDebugSettings-" + NLS::Core::Assets::AssetId::New().ToString());
    const auto result = NLS::Editor::Debug::GenerateVSCodeConfiguration(root, {}, 9911, true);
    ASSERT_TRUE(result.success) << result.errorMessage;

    std::ifstream launchInput(root / ".vscode" / "launch.json", std::ios::binary);
    const auto launch = nlohmann::json::parse(launchInput);
    ASSERT_EQ(launch["configurations"].size(), 3u);
    EXPECT_EQ(launch["configurations"][1]["connectionPort"], 9911);
    EXPECT_TRUE(launch["configurations"][1]["stopOnEntry"].get<bool>());
    launchInput.close();
    std::filesystem::remove_all(root);
}

TEST(ScriptRuntimeTests, ProjectDebugWorkspaceIsDeterministicAndUsesBrokerCommand)
{
    const auto repositoryRoot = FindNullusRoot();
    ASSERT_FALSE(repositoryRoot.empty());
    const auto projectRoot = repositoryRoot /
        ("tmp-debug-workspace-test-" + NLS::Core::Assets::AssetId::New().ToString());
    std::filesystem::create_directories(projectRoot / "Assets");
    {
        std::ofstream projectFile(projectRoot / "TestProject.nullus", std::ios::binary);
        projectFile << "{}\n";
    }

    const auto editor = repositoryRoot / "App" / "Win64_Debug_Runtime_Shared" / "Editor.exe";
    const auto broker = repositoryRoot / "App" / "Win64_Debug_Runtime_Shared" / "NullusDebugBroker.exe";
    const auto first = NLS::Editor::Debug::GenerateProjectDebugWorkspace(projectRoot, editor, broker);
    ASSERT_TRUE(first.success) << first.errorMessage;
    const auto second = NLS::Editor::Debug::GenerateProjectDebugWorkspace(projectRoot, editor, broker);
    ASSERT_TRUE(second.success) << second.errorMessage;
    EXPECT_EQ(first.manifest.projectId, second.manifest.projectId);
    EXPECT_EQ(first.manifest.workspaceRoot, second.manifest.workspaceRoot);
    auto editorPdb = editor;
    editorPdb.replace_extension(".pdb");
    EXPECT_EQ(first.manifest.nativeSymbolsAvailable,
        std::filesystem::is_regular_file(editorPdb));
    EXPECT_EQ(first.manifest.mixedDebugAvailable,
        first.manifest.engineSourceAvailable && first.manifest.nativeSymbolsAvailable &&
            !first.manifest.editorExecutable.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(
        first.manifest.visualStudioSolution.parent_path() / "Nullus.Debug.vs.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(first.manifest.visualStudioScriptsFilter));
    EXPECT_TRUE(std::filesystem::is_regular_file(first.manifest.visualStudioEngineFilter));

    const auto launch = nlohmann::json::parse(ReadTextFile(projectRoot / ".vscode" / "launch.json"));
    const auto csharp = std::find_if(
        launch["configurations"].begin(),
        launch["configurations"].end(),
        [](const auto& value) { return value.value("name", "") == "Nullus: C# Scripts"; });
    ASSERT_NE(csharp, launch["configurations"].end());
    EXPECT_EQ(csharp->value("processId", ""), "${command:nullus.resolveEditorProcess}");
    const auto generatedProject = ReadTextFile(first.manifest.visualStudioProject);
    EXPECT_EQ(generatedProject.find("StartAction"), std::string::npos);
    EXPECT_EQ(generatedProject.find("<ProjectReference"), std::string::npos);
    EXPECT_EQ(generatedProject.find("Nullus.GameScripts\\Scripts"), std::string::npos);
    EXPECT_NE(generatedProject.find("<Reference Include=\"Nullus.Managed\">"), std::string::npos);
    EXPECT_EQ(generatedProject.find("Nullus.ScriptGenerator.csproj"), std::string::npos);
    const auto generatedSolution = ReadTextFile(first.manifest.visualStudioSolution);
    EXPECT_EQ(generatedSolution.find("Nullus.ScriptGenerator"), std::string::npos);
    const auto hasNativeVcxproj = [&first](const char* name)
    {
        return std::any_of(
            first.manifest.engineProjects.begin(),
        first.manifest.engineProjects.end(),
        [&first, name](const auto& project)
            {
                return project.name == name &&
                    std::filesystem::is_regular_file(first.manifest.engineBuildRoot / project.path);
            });
    };
    const auto expectNativeProject = [&generatedSolution, &hasNativeVcxproj](const char* name)
    {
        const std::string marker =
            std::string("Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"") + name + "\"";
        if (hasNativeVcxproj(name))
            EXPECT_NE(generatedSolution.find(marker), std::string::npos);
        else
            EXPECT_EQ(generatedSolution.find(marker), std::string::npos);
    };
    expectNativeProject("Editor");
    expectNativeProject("NLS_Scripting");

    const auto roundTrip = NLS::Editor::Debug::ReadProjectDebugManifest(projectRoot);
    ASSERT_TRUE(roundTrip.has_value());
    EXPECT_EQ(roundTrip->projectId, first.manifest.projectId);
    std::filesystem::remove_all(projectRoot);
}

TEST(ScriptRuntimeTests, VisualStudioConfigurationGeneratesProjectScopedF5Configuration)
{
    const auto repositoryRoot = FindNullusRoot();
    ASSERT_FALSE(repositoryRoot.empty());
    const auto root = repositoryRoot /
        ("tmp-visual-studio-debug-" + NLS::Core::Assets::AssetId::New().ToString());
    const auto editor = repositoryRoot / "App" / "Win64_Debug_Runtime_Shared" / "Editor.exe";
    std::filesystem::create_directories(root / "Assets");
    {
        std::ofstream projectFile(root / "TestProject.nullus", std::ios::binary);
        projectFile << "{}\n";
    }

    const auto result = NLS::Editor::Debug::GenerateVisualStudioConfiguration(root, editor);
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_TRUE(std::filesystem::exists(result.launchProfilePath));
    ASSERT_TRUE(std::filesystem::exists(result.projectUserPath));
    const auto projectUser = ReadTextFile(result.projectUserPath);
    EXPECT_NE(projectUser.find("<NullusDebugProvider>NullusEditor</NullusDebugProvider>"), std::string::npos);
    EXPECT_EQ(projectUser.find("StartAction"), std::string::npos);
    const auto launch = nlohmann::json::parse(ReadTextFile(result.launchProfilePath));
    ASSERT_TRUE(launch.contains("profiles"));
    EXPECT_EQ(launch["profiles"]["Nullus: C# Scripts"]["commandName"], "NullusEditor");
    EXPECT_FALSE(launch["profiles"]["Nullus: C# Scripts"]["nullusPlayAfterAttach"].get<bool>());
    EXPECT_TRUE(launch["profiles"].contains("Nullus: C# Attach and Play"));
    const auto manifest = NLS::Editor::Debug::ReadProjectDebugManifest(root);
    ASSERT_TRUE(manifest.has_value());
    if (manifest->mixedDebugAvailable)
    {
        EXPECT_TRUE(launch["profiles"].contains("Nullus: Editor + C# Mixed"));
        EXPECT_TRUE(launch["profiles"].contains("Nullus: Editor + C# Mixed (Release)"));
        EXPECT_EQ(launch["profiles"]["Nullus: Editor + C# Mixed"]["nullusConfiguration"], "Debug");
        EXPECT_EQ(launch["profiles"]["Nullus: Editor + C# Mixed (Release)"]["nullusConfiguration"], "Release");
    }
    else
    {
        EXPECT_FALSE(launch["profiles"].contains("Nullus: Editor + C# Mixed"));
        EXPECT_FALSE(launch["profiles"].contains("Nullus: Editor + C# Mixed (Release)"));
    }
    EXPECT_TRUE(result.guidePath.empty());
    EXPECT_TRUE(result.attachScriptPath.empty());
    std::filesystem::remove_all(root);
}

TEST(ScriptRuntimeTests, ManagedScriptBuildUsesRepositoryLocalDotnetWithoutRestore)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty());
    const auto command = NLS::Editor::Scripting::ManagedScriptBuildService::BuildCommand({root});
    EXPECT_NE(command.find("dotnet"), std::string::npos);
    EXPECT_NE(command.find("--no-restore"), std::string::npos);
    EXPECT_NE(command.find("DebugType=portable"), std::string::npos);
    EXPECT_NE(command.find("Optimize=false"), std::string::npos);
}

TEST(ScriptRuntimeTests, ManagedScriptDebugBuildRunsWithRepositoryLocalSdk)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty());
    const auto outputRoot = std::filesystem::temp_directory_path() /
        ("NullusManagedDebugBuild-" + NLS::Core::Assets::AssetId::New().ToString());
    NLS::Editor::Scripting::ManagedScriptBuildRequest request;
    request.projectRoot = root;
    request.outputRoot = outputRoot;
    request.configuration = "Debug";

    const auto result = NLS::Editor::Scripting::ManagedScriptBuildService::Build(request);
    ASSERT_TRUE(result.succeeded) << result.output;
    EXPECT_TRUE(std::filesystem::exists(result.assembly));
    EXPECT_TRUE(std::filesystem::exists(result.symbols));
    EXPECT_TRUE(std::filesystem::exists(result.runtimeConfig));
    EXPECT_TRUE(std::filesystem::exists(outputRoot / "Nullus.Managed.dll"));
    EXPECT_TRUE(std::filesystem::exists(outputRoot / "Nullus.Managed.deps.json"));
    EXPECT_NE(result.assembly.filename().string().find("GameScripts."), std::string::npos);
    std::filesystem::remove_all(outputRoot);
}

TEST(ScriptRuntimeTests, FailedManagedScriptBuildKeepsPreviousHashedAssembly)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty());
    const auto outputRoot = std::filesystem::temp_directory_path() /
        ("NullusScriptBuildFailure-" + NLS::Core::Assets::AssetId::New().ToString());
    std::filesystem::create_directories(outputRoot);
    const auto previousAssembly = outputRoot / "GameScripts.deadbeef.dll";
    {
        std::ofstream output(previousAssembly, std::ios::binary);
        output << "previous assembly";
    }

    NLS::Editor::Scripting::ManagedScriptBuildRequest request;
    request.projectRoot = root;
    request.outputRoot = outputRoot;
    request.dotnetPath = outputRoot / "missing-dotnet.exe";
    const auto result = NLS::Editor::Scripting::ManagedScriptBuildService::Build(request);
    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(std::filesystem::exists(previousAssembly));
    EXPECT_EQ(ReadTextFile(previousAssembly), "previous assembly");
    std::filesystem::remove_all(outputRoot);
}

TEST(ScriptRuntimeTests, RuntimeSetupDiscoversContentAddressedManagedArtifacts)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty());
    const auto projectRoot = std::filesystem::temp_directory_path() /
        ("NullusScriptArtifactDiscovery-" + NLS::Core::Assets::AssetId::New().ToString());
    const auto outputRoot = projectRoot / "Library" / "ScriptBuild" / "Managed";
    std::filesystem::create_directories(outputRoot);
    const auto assembly = outputRoot / "GameScripts.0123456789abcdef.dll";
    const auto runtimeConfig = outputRoot / "GameScripts.0123456789abcdef.runtimeconfig.json";
    {
        std::ofstream assemblyOutput(assembly, std::ios::binary);
        assemblyOutput << "placeholder";
        std::ofstream configOutput(runtimeConfig, std::ios::binary);
        configOutput << "{}";
    }

    ScriptRuntime runtime;
    ScriptRuntimeSetupOptions options;
    options.projectRoot = projectRoot;
    options.scriptApiDirectory = root / "Library" / "ScriptApi";
    options.dotnetRoot = root / "Tools" / "Dotnet" / "windows" / "x64";
    options.enableCSharp = false;
    options.enableLua = false;
    const auto setup = InitializeScriptRuntime(runtime, options);
    ASSERT_TRUE(setup.status.Succeeded()) << setup.status.message;
    EXPECT_TRUE(std::filesystem::equivalent(setup.managedAssembly, assembly));
    EXPECT_TRUE(std::filesystem::equivalent(setup.managedRuntimeConfig, runtimeConfig));
    EXPECT_FALSE(setup.csharpRegistered);
    EXPECT_FALSE(setup.luaRegistered);
    std::filesystem::remove_all(projectRoot);
}

namespace
{
ScriptApiDatabase MakeSerializedFieldApi(ScriptFieldId fieldId = 42)
{
    ScriptApiDatabase api;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Scripts::Inspectable";
    descriptor.id = MakeStableScriptId(descriptor.name);
    ScriptFieldDescriptor field;
    field.id = fieldId;
    field.name = "speed";
    field.type = {0, ScriptValueKind::Int32, "int32_t", 4, 4, true};
    field.defaultValue = ScriptValue{int32_t{3}};
    descriptor.fields.push_back(std::move(field));
    EXPECT_TRUE(api.RegisterType(std::move(descriptor)));
    return api;
}

ScriptAsset MakeInspectableAsset(const ScriptApiDatabase& api)
{
    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    asset.scriptType = api.GetTypes().front().id;
    asset.sourcePath = "Assets/Inspectable.lua";
    asset.sourceText = "return {}";
    return asset;
}

ScriptApiDatabase MakePersistenceApi()
{
    ScriptApiDatabase api;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Scripts::Persistence";
    descriptor.id = MakeStableScriptId(descriptor.name);

    ScriptFieldDescriptor value;
    value.id = 101;
    value.name = "newValue";
    value.aliases = {"oldValue"};
    value.type = {0, ScriptValueKind::Int32, "int32_t", 4, 4, true};
    value.defaultValue = ScriptValue{int32_t{7}};
    descriptor.fields.push_back(std::move(value));

    ScriptFieldDescriptor target;
    target.id = 102;
    target.name = "target";
    target.type = {0, ScriptValueKind::ObjectReference, "NLS::Engine::Object", 0, 0, true};
    descriptor.fields.push_back(std::move(target));
    EXPECT_TRUE(api.RegisterType(std::move(descriptor)));
    return api;
}

ScriptAsset MakePersistenceAsset(const ScriptApiDatabase& api)
{
    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    asset.scriptType = api.GetTypes().front().id;
    asset.sourcePath = "Assets/Persistence.lua";
    asset.sourceText = "return { oldValue = 1 }";
    asset.contentHash = MakeScriptContentHash(asset.sourceText);
    return asset;
}
}

TEST(ScriptRuntimeTests, SerializedObjectFindsFieldsAndAppliesOnlyOnCommit)
{
    auto backend = std::make_unique<RecordingBackend>();
    auto* backendPtr = backend.get();
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    const auto api = MakeSerializedFieldApi();
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());

    ScriptComponent component;
    component.SetRuntime(&runtime);
    component.SetScriptAsset(MakeInspectableAsset(api));
    SerializedObject serialized(component);
    auto* property = serialized.FindProperty("speed");
    ASSERT_NE(property, nullptr);
    ASSERT_TRUE(property->IsValid());
    ASSERT_TRUE(std::holds_alternative<int32_t>(property->GetValue()));
    EXPECT_EQ(std::get<int32_t>(property->GetValue()), 3);
    ASSERT_TRUE(property->SetValue(int32_t{9}));
    EXPECT_TRUE(serialized.HasModifiedProperties());
    EXPECT_EQ(component.FindSerializedField(42), nullptr);
    EXPECT_TRUE(serialized.ApplyModifiedProperties());
    ASSERT_NE(component.FindSerializedField(42), nullptr);
    EXPECT_EQ(std::get<int32_t>(*component.FindSerializedField(42)), 9);
    EXPECT_FALSE(serialized.HasModifiedProperties());
    EXPECT_TRUE(backendPtr->fields.empty());
}

TEST(ScriptRuntimeTests, SerializedObjectUpdateInvalidatesOldPropertyAndDiscardsChanges)
{
    ScriptRuntime runtime;
    const auto api = MakeSerializedFieldApi();
    ASSERT_TRUE(runtime.RegisterBackend(std::make_unique<RecordingBackend>()).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());
    ScriptComponent component;
    component.SetRuntime(&runtime);
    component.SetScriptAsset(MakeInspectableAsset(api));

    SerializedObject serialized(component);
    auto* oldProperty = serialized.FindProperty(42);
    ASSERT_NE(oldProperty, nullptr);
    ASSERT_TRUE(oldProperty->SetValue(int32_t{11}));
    serialized.Update();
    EXPECT_FALSE(oldProperty->IsValid());
    auto* currentProperty = serialized.FindProperty("speed");
    ASSERT_NE(currentProperty, nullptr);
    EXPECT_EQ(std::get<int32_t>(currentProperty->GetValue()), 3);
    EXPECT_FALSE(serialized.HasModifiedProperties());
}

TEST(ScriptRuntimeTests, SerializedObjectIteratorWalksVisibleFields)
{
    ScriptApiDatabase api;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Scripts::Iterator";
    descriptor.id = MakeStableScriptId(descriptor.name);
    ScriptFieldDescriptor first;
    first.id = 1;
    first.name = "first";
    first.type = {0, ScriptValueKind::Int32, "int32_t", 4, 4, true};
    first.defaultValue = ScriptValue{int32_t{1}};
    ScriptFieldDescriptor second;
    second.id = 2;
    second.name = "second";
    second.type = {0, ScriptValueKind::Bool, "bool", 1, 1, true};
    second.defaultValue = ScriptValue{true};
    descriptor.fields = {std::move(first), std::move(second)};
    ASSERT_TRUE(api.RegisterType(std::move(descriptor)));

    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::make_unique<RecordingBackend>()).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());
    ScriptComponent component;
    component.SetRuntime(&runtime);
    component.SetScriptAsset(MakeInspectableAsset(api));
    SerializedObject serialized(component);
    auto* iterator = serialized.GetIterator();
    ASSERT_NE(iterator, nullptr);
    EXPECT_EQ(iterator->GetPropertyPath(), "first");
    iterator = iterator->NextVisible(false);
    ASSERT_NE(iterator, nullptr);
    EXPECT_EQ(iterator->GetPropertyPath(), "second");
    EXPECT_EQ(iterator->NextVisible(false), nullptr);
}

TEST(ScriptRuntimeTests, SerializedPropertyChecksTypeDefaultsAndRuntimeHandlePersistence)
{
    ScriptApiDatabase api;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Scripts::Types";
    descriptor.id = MakeStableScriptId(descriptor.name);
    ScriptFieldDescriptor field;
    field.id = 7;
    field.name = "value";
    field.type = {0, ScriptValueKind::Int32, "int32_t", 4, 4, true};
    field.defaultValue = ScriptValue{int32_t{17}};
    descriptor.fields.push_back(std::move(field));
    ASSERT_TRUE(api.RegisterType(std::move(descriptor)));

    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::make_unique<RecordingBackend>()).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());
    ScriptComponent component;
    component.SetRuntime(&runtime);
    component.SetScriptAsset(MakeInspectableAsset(api));
    SerializedObject serialized(component);
    auto* property = serialized.FindProperty(7);
    ASSERT_NE(property, nullptr);
    EXPECT_FALSE(property->SetValue(std::string("wrong")));
    EXPECT_TRUE(property->SetValue(int32_t{23}));
    EXPECT_TRUE(property->ResetToDefault());
    EXPECT_EQ(std::get<int32_t>(property->GetValue()), 17);
    EXPECT_FALSE(property->SetValue(NativeObjectHandle::FromInstanceId(12)));
}

TEST(ScriptRuntimeTests, SerializedObjectApplyUpdatesLiveScriptInstance)
{
    auto backend = std::make_unique<RecordingBackend>();
    auto* backendPtr = backend.get();
    ScriptRuntime runtime;
    const auto api = MakeSerializedFieldApi();
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());

    ScriptComponent component;
    component.SetRuntime(&runtime);
    component.SetScriptAsset(MakeInspectableAsset(api));
    component.OnAwake();
    SerializedObject serialized(component);
    ASSERT_TRUE(serialized.FindProperty(42)->SetValue(int32_t{31}));
    ASSERT_TRUE(serialized.ApplyModifiedProperties());
    ASSERT_NE(backendPtr->fields.find(42), backendPtr->fields.end());
    EXPECT_EQ(std::get<int32_t>(backendPtr->fields.at(42)), 31);
    component.OnDestroy();
}

TEST(ScriptRuntimeTests, SerializedObjectReadsLiveRuntimeValueWithoutPersistingPlayChanges)
{
    auto backend = std::make_unique<RecordingBackend>();
    auto* backendPtr = backend.get();
    ScriptRuntime runtime;
    const auto api = MakeSerializedFieldApi();
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());

    ScriptComponent component;
    component.SetRuntime(&runtime);
    component.SetScriptAsset(MakeInspectableAsset(api));
    component.OnAwake();
    SerializedObject serialized(component);
    auto* property = serialized.FindProperty("speed");
    ASSERT_NE(property, nullptr);

    // Simulate a script changing its field during Play.  Inspector reads must
    // observe it, while the scene override map remains untouched.
    backendPtr->fields[42] = int32_t{17};
    EXPECT_EQ(std::get<int32_t>(property->GetValue()), 17);
    EXPECT_EQ(component.FindSerializedField(42), nullptr);
    EXPECT_FALSE(serialized.HasModifiedProperties());

    // Stopping Play invalidates the live instance; the Inspector must return
    // to the schema default rather than retaining the transient value.
    component.OnDestroy();
    EXPECT_EQ(std::get<int32_t>(property->GetValue()), 3);
    EXPECT_EQ(component.FindSerializedField(42), nullptr);

    component.OnCreate();
    component.OnAwake();

    // A value currently being edited in Inspector must not be overwritten by
    // a subsequent script tick until the edit is committed.
    ASSERT_TRUE(property->SetValue(int32_t{23}));
    backendPtr->fields[42] = int32_t{31};
    EXPECT_EQ(std::get<int32_t>(property->GetValue()), 23);
    ASSERT_TRUE(serialized.ApplyModifiedProperties());
    EXPECT_EQ(std::get<int32_t>(backendPtr->fields.at(42)), 23);
    EXPECT_EQ(std::get<int32_t>(*component.FindSerializedField(42)), 23);

    component.OnDestroy();
}

TEST(ScriptRuntimeTests, ScriptComponentSceneAndPrefabRoundTripPersistsOnlyStableState)
{
    ScriptRuntime runtime;
    const auto api = MakePersistenceApi();
    ASSERT_TRUE(runtime.RegisterBackend(std::make_unique<RecordingBackend>()).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());

    NLS::Engine::GameObject root("PersistedRoot");
    auto* script = root.AddComponent<ScriptComponent>([&](auto* component)
    {
        auto* scriptComponent = static_cast<ScriptComponent*>(component);
        scriptComponent->SetRuntime(&runtime);
        scriptComponent->SetScriptAsset(MakePersistenceAsset(api));
    });
    ASSERT_NE(script, nullptr);
    ASSERT_TRUE(script->SetSerializedField(101, int32_t{23}));
    ASSERT_TRUE(script->SetSerializedField(102, ScriptObjectReference{
        "00112233445566778899aabbccddeeff", 42, 2, "Assets/Target.prefab"}));
    script->GetOrphanFields()[999] = R"({"kind":"scalar","value":99})";

    const auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);
    const auto componentRecord = std::find_if(
        prefab.graph.objects.begin(),
        prefab.graph.objects.end(),
        [](const auto& record)
        {
            return record.typeName == "NLS::Scripting::ScriptComponent";
        });
    ASSERT_NE(componentRecord, prefab.graph.objects.end());
    const auto hasProperty = [&](std::string_view name)
    {
        return std::find_if(componentRecord->properties.begin(), componentRecord->properties.end(),
            [name](const auto& property) { return property.name == name; }) != componentRecord->properties.end();
    };
    EXPECT_TRUE(hasProperty("assetId"));
    EXPECT_TRUE(hasProperty("scriptTypeId"));
    EXPECT_FALSE(hasProperty("sourceText"));
    EXPECT_FALSE(hasProperty("sourcePath"));

    NLS::Engine::SceneSystem::Scene scene;
    const auto instantiated = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiatePrefabGraph(
        prefab.graph, scene, {});
    ASSERT_NE(instantiated.root, nullptr);
    auto* restored = instantiated.root->GetComponent<ScriptComponent>();
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->GetScriptAsset().assetId, script->GetScriptAsset().assetId);
    EXPECT_EQ(restored->GetScriptAsset().language, ScriptLanguage::Lua);
    EXPECT_EQ(restored->GetScriptAsset().scriptType, script->GetScriptAsset().scriptType);
    EXPECT_TRUE(restored->GetScriptAsset().sourcePath.empty());
    EXPECT_TRUE(restored->GetScriptAsset().sourceText.empty());

    restored->SetRuntime(&runtime);
    ASSERT_NE(restored->FindSerializedField(101), nullptr);
    EXPECT_EQ(std::get<int32_t>(*restored->FindSerializedField(101)), 23);
    ASSERT_NE(restored->FindSerializedField(102), nullptr);
    EXPECT_EQ(std::get<ScriptObjectReference>(*restored->FindSerializedField(102)),
              std::get<ScriptObjectReference>(*script->FindSerializedField(102)));
    EXPECT_TRUE(restored->GetOrphanFields().contains(999));

    NLS::Engine::SceneSystem::Scene sourceScene;
    auto& sceneObject = sourceScene.CreateGameObject("ScenePersistedRoot");
    auto* sceneScript = sceneObject.AddComponent<ScriptComponent>([&](auto* component)
    {
        auto* scriptComponent = static_cast<ScriptComponent*>(component);
        scriptComponent->SetRuntime(&runtime);
        scriptComponent->SetScriptAsset(MakePersistenceAsset(api));
    });
    ASSERT_NE(sceneScript, nullptr);
    sceneScript->GetOrphanFields()[1000] = R"({"kind":"scalar","value":100})";

    const auto sceneDocument = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    const auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(sceneDocument);
    ASSERT_NE(loadedScene, nullptr);
    ASSERT_FALSE(loadedScene->GetGameObjects().empty());
    auto* sceneRestored = loadedScene->GetGameObjects().front()->GetComponent<ScriptComponent>();
    ASSERT_NE(sceneRestored, nullptr);
    sceneRestored->SetRuntime(&runtime);
    ASSERT_NE(sceneRestored->FindSerializedField(101), nullptr);
    EXPECT_EQ(std::get<int32_t>(*sceneRestored->FindSerializedField(101)), 7);
    EXPECT_TRUE(sceneRestored->GetOrphanFields().contains(1000));
}

TEST(ScriptRuntimeTests, ScriptComponentPersistenceMigratesAliasesAndRejectsRuntimeHandles)
{
    ScriptRuntime runtime;
    const auto api = MakePersistenceApi();
    ASSERT_TRUE(runtime.RegisterBackend(std::make_unique<RecordingBackend>()).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());

    ScriptComponent component;
    component.SetRuntime(&runtime);
    component.SetScriptAsset(MakePersistenceAsset(api));
    std::vector<NLS::Engine::Serialize::PropertyRecord> properties = {
        {"assetId", NLS::Engine::Serialize::PropertyValue::String(component.GetScriptAsset().assetId.ToString())},
        {"language", NLS::Engine::Serialize::PropertyValue::Integer(static_cast<int64_t>(ScriptLanguage::Lua))},
        {"scriptTypeId", NLS::Engine::Serialize::PropertyValue::String(std::to_string(component.GetScriptAsset().scriptType))},
        {"fields", NLS::Engine::Serialize::PropertyValue::String(
            R"({"oldValue":{"kind":"scalar","value":5},"1234":{"kind":"scalar","value":8}})")}
    };
    ASSERT_TRUE(component.DeserializeObjectGraphProperties(properties));
    ASSERT_NE(component.FindSerializedField(101), nullptr);
    EXPECT_EQ(std::get<int32_t>(*component.FindSerializedField(101)), 5);
    EXPECT_TRUE(component.GetOrphanFields().contains(1234));

    NLS::Engine::GameObject runtimeHandleRoot("RuntimeHandle");
    auto* runtimeHandleComponent = runtimeHandleRoot.AddComponent<ScriptComponent>([&](auto* value)
    {
        auto* script = static_cast<ScriptComponent*>(value);
        script->SetRuntime(&runtime);
        script->SetScriptAsset(MakePersistenceAsset(api));
    });
    ASSERT_NE(runtimeHandleComponent, nullptr);
    ASSERT_TRUE(runtimeHandleComponent->SetSerializedField(102, NativeObjectHandle::FromInstanceId(12)));
    EXPECT_THROW(
        NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(
            runtimeHandleRoot),
        std::invalid_argument);
}

TEST(ScriptRuntimeTests, RejectsDuplicateBackendAndInvalidHandles)
{
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::make_unique<RecordingBackend>()).Succeeded());
    EXPECT_FALSE(runtime.RegisterBackend(std::make_unique<RecordingBackend>()).Succeeded());
    EXPECT_TRUE(runtime.DestroyInstance({}).Succeeded());
    EXPECT_TRUE(runtime.Invoke({}, ScriptCallback::Update, {}).Succeeded());
}

TEST(ScriptRuntimeTests, RejectsForeignHandlesAndKeepsDestroyIdempotent)
{
    auto backend = std::make_unique<RecordingBackend>();
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize({}).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    ASSERT_TRUE(runtime.LoadScript(asset).Succeeded());

    ScriptInstanceHandle instance;
    ASSERT_TRUE(runtime.CreateInstance(asset, {}, instance).Succeeded());
    ASSERT_TRUE(runtime.DestroyInstance(instance).Succeeded());
    EXPECT_TRUE(runtime.DestroyInstance(instance).Succeeded());
    EXPECT_EQ(runtime.Invoke(instance, ScriptCallback::Update, {}).code, ScriptStatusCode::AlreadyDestroyed);
    EXPECT_EQ(runtime.Invoke({7, 99, 99}, ScriptCallback::Update, {}).code, ScriptStatusCode::InvalidHandle);
}

TEST(ScriptRuntimeTests, FakeBackendCanBeConfiguredForEitherLanguage)
{
    auto backend = std::make_unique<FakeScriptBackend>(ScriptBackendId{19}, ScriptLanguage::CSharp);
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize({}).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::CSharp;
    ASSERT_TRUE(runtime.LoadScript(asset).Succeeded());
    ScriptInstanceHandle instance;
    ASSERT_TRUE(runtime.CreateInstance(asset, {}, instance).Succeeded());
    EXPECT_TRUE(runtime.Invoke(instance, ScriptCallback::Awake, {}).Succeeded());
}

TEST(ScriptRuntimeTests, RuntimeCanRegisterAndInitializeBackendAfterStartup)
{
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.Initialize({}).Succeeded());

    auto backend = std::make_unique<FakeScriptBackend>(ScriptBackendId{29}, ScriptLanguage::CSharp);
    auto* backendPointer = backend.get();
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    EXPECT_TRUE(backendPointer->IsInitialized());
    EXPECT_NE(runtime.GetBackend(ScriptLanguage::CSharp), nullptr);
}

TEST(ScriptRuntimeTests, SchedulerBatchesOnlyContiguousBackendsAndKeepsOrder)
{
    auto csharp = std::make_unique<FakeScriptBackend>(ScriptBackendId{31}, ScriptLanguage::CSharp);
    auto lua = std::make_unique<FakeScriptBackend>(ScriptBackendId{32}, ScriptLanguage::Lua);
    auto* csharpTrace = csharp.get();
    auto* luaTrace = lua.get();
    std::vector<uint16_t> order;
    csharpTrace->SetInvocationObserver([&](auto, auto, const auto&) { order.push_back(31); });
    luaTrace->SetInvocationObserver([&](auto, auto, const auto&) { order.push_back(32); });

    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(csharp)).Succeeded());
    ASSERT_TRUE(runtime.RegisterBackend(std::move(lua)).Succeeded());
    ASSERT_TRUE(runtime.Initialize({}).Succeeded());

    ScriptAsset csharpAsset;
    csharpAsset.assetId = NLS::Core::Assets::AssetId::New();
    csharpAsset.language = ScriptLanguage::CSharp;
    csharpAsset.sourceText = "class CSharpBehaviour {}";
    ScriptAsset luaAsset;
    luaAsset.assetId = NLS::Core::Assets::AssetId::New();
    luaAsset.language = ScriptLanguage::Lua;
    luaAsset.sourceText = "return {}";
    ASSERT_TRUE(runtime.LoadScript(csharpAsset).Succeeded());
    ASSERT_TRUE(runtime.LoadScript(luaAsset).Succeeded());

    ScriptInstanceHandle csharpFirst;
    ScriptInstanceHandle luaInstance;
    ScriptInstanceHandle csharpLast;
    ASSERT_TRUE(runtime.CreateInstance(csharpAsset, {}, csharpFirst).Succeeded());
    ASSERT_TRUE(runtime.CreateInstance(luaAsset, {}, luaInstance).Succeeded());
    ASSERT_TRUE(runtime.CreateInstance(csharpAsset, {}, csharpLast).Succeeded());

    ScriptScheduler scheduler;
    scheduler.Enqueue(csharpFirst);
    scheduler.Enqueue(luaInstance);
    scheduler.Enqueue(csharpLast);
    ASSERT_TRUE(scheduler.Flush(runtime, ScriptCallback::Update, {}).Succeeded());
    EXPECT_EQ(order, (std::vector<uint16_t>{31, 32, 31}));
}

TEST(ScriptRuntimeTests, BatchKeepsFirstDiagnosticAndContinuesRemainingInstances)
{
    auto backend = std::make_unique<RecordingBackend>();
    auto* backendPtr = backend.get();
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize({}).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    ASSERT_TRUE(runtime.LoadScript(asset).Succeeded());

    std::array<ScriptInstanceHandle, 3> instances;
    for (auto& instance : instances)
        ASSERT_TRUE(runtime.CreateInstance(asset, {}, instance).Succeeded());
    backendPtr->failingInstances.insert(instances[0]);

    const auto status = runtime.InvokeBatch(ScriptCallback::Update, instances, {});
    EXPECT_EQ(status.code, ScriptStatusCode::RuntimeError);
    EXPECT_EQ(status.message, "batch failure");
    EXPECT_EQ(backendPtr->invokedInstances,
              std::vector<ScriptInstanceHandle>(instances.begin(), instances.end()));
    ASSERT_EQ(runtime.GetErrors().size(), 1u);
    EXPECT_EQ(runtime.GetErrors().front().message, "batch failure");
}

TEST(ScriptRuntimeTests, SchedulerScalesBatchBoundariesForOneToTenThousandScripts)
{
    for (const size_t scriptCount : {size_t{1}, size_t{1000}, size_t{10000}})
    {
        auto backend = std::make_unique<FakeScriptBackend>(
            ScriptBackendId{static_cast<uint16_t>(90 + (scriptCount == 1000 ? 1 : scriptCount == 10000 ? 2 : 0))},
            ScriptLanguage::Lua);
        auto* backendPtr = backend.get();
        ScriptRuntime runtime;
        ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
        ASSERT_TRUE(runtime.Initialize({}).Succeeded());

        ScriptAsset asset;
        asset.assetId = NLS::Core::Assets::AssetId::New();
        asset.language = ScriptLanguage::Lua;
        asset.sourcePath = "Assets/Scale.lua";
        asset.sourceText = "return {}";
        ASSERT_TRUE(runtime.LoadScript(asset).Succeeded());

        std::vector<ScriptInstanceHandle> instances;
        instances.reserve(scriptCount);
        for (size_t index = 0; index < scriptCount; ++index)
        {
            ScriptInstanceHandle instance;
            ASSERT_TRUE(runtime.CreateInstance(
                asset,
                NativeObjectHandle::FromInstanceId(static_cast<int32_t>(index + 1)),
                instance).Succeeded());
            instances.push_back(instance);
        }

        std::vector<ScriptInstanceHandle> batchOrder;
        backendPtr->SetInvocationObserver([&](ScriptInstanceHandle instance, auto, const auto&) {
            batchOrder.push_back(instance);
        });
        ScriptScheduler scheduler;
        for (const auto instance : instances)
            scheduler.Enqueue(instance);

        const auto batchStart = std::chrono::steady_clock::now();
        ASSERT_TRUE(scheduler.Flush(runtime, ScriptCallback::Update, {}).Succeeded());
        const auto batchElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - batchStart).count();
        RecordProperty(("scheduler_batch_us_" + std::to_string(scriptCount)).c_str(), batchElapsed);

        EXPECT_EQ(backendPtr->GetBatchCallCount(), 1u);
        EXPECT_EQ(backendPtr->GetBatchInstanceCount(), scriptCount);
        EXPECT_EQ(backendPtr->GetCallbacks().size(), scriptCount);
        EXPECT_EQ(batchOrder, instances);

        backendPtr->ClearTrace();
        std::vector<ScriptInstanceHandle> directOrder;
        backendPtr->SetInvocationObserver([&](ScriptInstanceHandle instance, auto, const auto&) {
            directOrder.push_back(instance);
        });
        const auto directStart = std::chrono::steady_clock::now();
        for (const auto instance : instances)
            ASSERT_TRUE(runtime.Invoke(instance, ScriptCallback::Update, {}).Succeeded());
        const auto directElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - directStart).count();
        RecordProperty(("scheduler_direct_us_" + std::to_string(scriptCount)).c_str(), directElapsed);

        EXPECT_EQ(backendPtr->GetBatchCallCount(), 0u);
        EXPECT_EQ(directOrder, instances);
        EXPECT_EQ(backendPtr->GetCallbacks().size(), scriptCount);
    }
}

TEST(ScriptRuntimeTests, StableApiIdsAndSchemaHashAreDeterministic)
{
    ScriptApiDatabase left;
    ScriptApiDatabase right;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Engine::GameObject";
    descriptor.id = MakeStableScriptId(descriptor.name);
    descriptor.methods.push_back({MakeStableScriptId("NLS::Engine::GameObject::SetActive(bool)"), "SetActive", {}, {}, 0});
    ASSERT_TRUE(left.RegisterType(descriptor));
    ASSERT_TRUE(right.RegisterType(descriptor));
    EXPECT_EQ(left.GetSchemaHash(), right.GetSchemaHash());
    EXPECT_EQ(MakeStableScriptId(descriptor.name), descriptor.id);
}

TEST(ScriptRuntimeTests, RejectsDuplicateMemberIdsAndRoundTripsManifest)
{
    ScriptApiDatabase database;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Engine::Transform";
    descriptor.methods.push_back({MakeStableScriptId("duplicate"), "A", {}, {}, 0});
    descriptor.methods.push_back({MakeStableScriptId("duplicate"), "B", {}, {}, 0});
    EXPECT_FALSE(database.RegisterType(descriptor));

    descriptor.methods.clear();
    descriptor.methods.push_back({});
    descriptor.methods[0].name = "SetPosition";
    descriptor.methods[0].parameters.push_back({ScriptType{0, ScriptValueKind::Vector3, "NLS::Maths::Vector3"}, "value"});
    descriptor.fields.push_back({0, "position", {0, ScriptValueKind::Vector3, "NLS::Maths::Vector3"}});
    ASSERT_TRUE(database.RegisterType(descriptor));
    const auto json = ScriptApiManifest::ToJson(database);
    ScriptApiDatabase restored;
    ASSERT_TRUE(ScriptApiManifest::FromJson(json, restored).Succeeded());
    EXPECT_EQ(database.GetSchemaHashHex(), restored.GetSchemaHashHex());
}

TEST(ScriptRuntimeTests, ManifestRoundTripsFieldDefaults)
{
    ScriptApiDatabase database;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Scripts::Defaults";
    descriptor.fields.push_back({21, "speed", {0, ScriptValueKind::Float, "float", 4, 4, true}, true, {}, 1.25f});
    ASSERT_TRUE(database.RegisterType(descriptor));

    ScriptApiDatabase restored;
    ASSERT_TRUE(ScriptApiManifest::FromJson(ScriptApiManifest::ToJson(database), restored).Succeeded());
    const auto* type = restored.FindType("NLS::Scripts::Defaults");
    ASSERT_NE(type, nullptr);
    ASSERT_EQ(type->fields.size(), 1u);
    ASSERT_TRUE(type->fields[0].defaultValue.has_value());
    ASSERT_TRUE(std::holds_alternative<float>(*type->fields[0].defaultValue));
    EXPECT_FLOAT_EQ(std::get<float>(*type->fields[0].defaultValue), 1.25f);
}

TEST(ScriptRuntimeTests, RejectsInconsistentPropertyAccessors)
{
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Engine::PropertyContract";
    const ScriptType valueType{0, ScriptValueKind::Int32, "int32_t", 4, 4, true};
    const auto getterId = MakeStableScriptId("NLS::Engine::PropertyContract::GetValue()");
    const auto setterId = MakeStableScriptId("NLS::Engine::PropertyContract::SetValue(int32_t)");
    descriptor.methods.push_back({getterId, "GetValue", valueType, {}, 0, true, false, "GetValue()"});
    descriptor.methods.push_back({setterId, "SetValue", {}, {{valueType, "value"}}, 0, true, false, "SetValue(int32_t)"});
    descriptor.properties.push_back({
        MakeStableScriptId("NLS::Engine::PropertyContract::value::property"),
        "value",
        valueType,
        true,
        true,
        getterId,
        setterId,
        true});
    ScriptApiDatabase valid;
    EXPECT_TRUE(valid.RegisterType(descriptor));

    descriptor.properties[0].readable = false;
    ScriptApiDatabase inconsistent;
    EXPECT_FALSE(inconsistent.RegisterType(descriptor));
}

TEST(ScriptRuntimeTests, SerializesPortableValuesAndPreservesOrphans)
{
    SerializedScriptFields fields;
    fields.emplace(11, NLS::Maths::Vector3(1.0f, 2.0f, 3.0f));
    OrphanScriptFields orphan;
    orphan.emplace(99, "{\"kind\":\"scalar\",\"value\":7}");
    std::string json;
    ASSERT_TRUE(ScriptFieldSerialization::Serialize(fields, orphan, json).Succeeded());

    ScriptTypeDescriptor descriptor;
    descriptor.fields.push_back({11, "position", {0, ScriptValueKind::Vector3, "NLS::Maths::Vector3"}});
    SerializedScriptFields restored;
    OrphanScriptFields restoredOrphan;
    ASSERT_TRUE(ScriptFieldSerialization::Deserialize(json, &descriptor, restored, restoredOrphan).Succeeded());
    EXPECT_EQ(restored.size(), 1u);
    EXPECT_EQ(restoredOrphan.size(), 1u);
}

TEST(ScriptRuntimeTests, SerializesAndRestoresPersistentObjectReferences)
{
    const ScriptObjectReference reference{
        "00112233445566778899aabbccddeeff",
        42,
        2,
        "Assets/Player.prefab"};
    SerializedScriptFields fields;
    fields.emplace(17, reference);

    std::string json;
    ASSERT_TRUE(ScriptFieldSerialization::Serialize(fields, {}, json).Succeeded());
    EXPECT_NE(json.find("objectIdentifier"), std::string::npos);

    ScriptTypeDescriptor descriptor;
    descriptor.fields.push_back({17, "target", {0, ScriptValueKind::ObjectReference, "NLS::Engine::Object"}});
    SerializedScriptFields restored;
    OrphanScriptFields orphan;
    ASSERT_TRUE(ScriptFieldSerialization::Deserialize(json, &descriptor, restored, orphan).Succeeded());
    ASSERT_TRUE(restored.contains(17));
    ASSERT_TRUE(std::holds_alternative<ScriptObjectReference>(restored.at(17)));
    EXPECT_EQ(std::get<ScriptObjectReference>(restored.at(17)), reference);
    EXPECT_TRUE(orphan.empty());
}

TEST(ScriptRuntimeTests, RejectsRuntimeHandlesDuringPersistence)
{
    SerializedScriptFields fields;
    fields.emplace(17, NativeObjectHandle::FromInstanceId(123));
    std::string json;
    const auto status = ScriptFieldSerialization::Serialize(fields, {}, json);
    EXPECT_EQ(status.code, ScriptStatusCode::InvalidArgument);
    EXPECT_TRUE(json.empty());

    ScriptTypeDescriptor descriptor;
    descriptor.fields.push_back({17, "target", {0, ScriptValueKind::ObjectReference, "NLS::Engine::Object"}});
    SerializedScriptFields restored;
    OrphanScriptFields orphan;
    ASSERT_TRUE(ScriptFieldSerialization::Deserialize(
        R"({"17":{"kind":"runtimeObject","value":123}})",
        &descriptor,
        restored,
        orphan)
                    .Succeeded());
    EXPECT_TRUE(restored.empty());
    ASSERT_TRUE(orphan.contains(17));

    EXPECT_EQ(ScriptFieldSerialization::Serialize({}, orphan, json).code, ScriptStatusCode::InvalidArgument);
}

TEST(ScriptRuntimeTests, SchemaHashIsIndependentOfDeclarationOrder)
{
    ScriptTypeDescriptor first;
    first.name = "NLS::Engine::Ordered";
    first.methods.push_back({0, "Second", {}, {}, 0, false, false, "Second()"});
    first.methods.push_back({0, "First", {}, {}, 0, false, false, "First()"});

    ScriptTypeDescriptor second = first;
    std::swap(second.methods[0], second.methods[1]);

    ScriptApiDatabase left;
    ScriptApiDatabase right;
    ASSERT_TRUE(left.RegisterType(first));
    ASSERT_TRUE(right.RegisterType(second));
    EXPECT_EQ(left.GetSchemaHashHex(), right.GetSchemaHashHex());
}

TEST(ScriptRuntimeTests, FieldAliasesAndDefaultsAreMigrated)
{
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Scripts::Migrated";
    ScriptFieldDescriptor field;
    field.id = 21;
    field.name = "newValue";
    field.aliases = {"oldValue"};
    field.type = {0, ScriptValueKind::Int32, "int32_t", 4, 4, true};
    field.defaultValue = int32_t{9};
    descriptor.fields.push_back(field);

    SerializedScriptFields restored;
    OrphanScriptFields orphan;
    ASSERT_TRUE(ScriptFieldSerialization::Deserialize(
        R"({"oldValue":{"kind":"scalar","value":3}})",
        &descriptor,
        restored,
        orphan)
                    .Succeeded());
    ASSERT_TRUE(restored.contains(21));
    EXPECT_EQ(std::get<int32_t>(restored.at(21)), 3);
    EXPECT_TRUE(orphan.empty());

    restored.clear();
    ASSERT_TRUE(ScriptFieldSerialization::Deserialize("{}", &descriptor, restored, orphan).Succeeded());
    ASSERT_TRUE(restored.contains(21));
    EXPECT_EQ(std::get<int32_t>(restored.at(21)), 9);
}

TEST(ScriptRuntimeTests, FailedReloadRestoresTheAcceptedArtifact)
{
    auto backend = std::make_unique<RecordingBackend>();
    auto* backendPtr = backend.get();
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize({}).Succeeded());

    ScriptAsset original;
    original.language = ScriptLanguage::Lua;
    original.sourceText = "return { version = 1 }";
    ASSERT_TRUE(runtime.LoadScript(original).Succeeded());

    backendPtr->rejectReload = true;
    ScriptAsset replacement = original;
    replacement.sourceText = "return { version = 2 }";
    const auto status = runtime.Reload(replacement, {});
    EXPECT_EQ(status.code, ScriptStatusCode::HotReloadRejected);
    ASSERT_EQ(backendPtr->loadedSources.size(), 3u);
    EXPECT_EQ(backendPtr->loadedSources[0], original.sourceText);
    EXPECT_EQ(backendPtr->loadedSources[1], replacement.sourceText);
    EXPECT_EQ(backendPtr->loadedSources[2], original.sourceText);
}

TEST(ScriptRuntimeTests, ReloadSnapshotsCompatibleFieldsAndOnlyRebindsLifecycle)
{
    ScriptApiDatabase api;
    ScriptTypeDescriptor oldType;
    oldType.name = "NLS::Scripts::ReloadOld";
    oldType.id = MakeStableScriptId(oldType.name);
    oldType.fields.push_back({42, "oldValue", {0, ScriptValueKind::Int32, "int32_t", 4, 4, true}});
    ScriptTypeDescriptor newType;
    newType.name = "NLS::Scripts::ReloadNew";
    newType.id = MakeStableScriptId(newType.name);
    ScriptFieldDescriptor renamed{43, "newValue", {0, ScriptValueKind::Int32, "int32_t", 4, 4, true}};
    renamed.aliases = {"oldValue"};
    newType.fields.push_back(renamed);
    ASSERT_TRUE(api.RegisterType(oldType));
    ASSERT_TRUE(api.RegisterType(newType));

    auto backend = std::make_unique<RecordingBackend>();
    auto* backendPtr = backend.get();
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());

    ScriptAsset original;
    original.assetId = NLS::Core::Assets::AssetId::New();
    original.language = ScriptLanguage::Lua;
    original.scriptType = oldType.id;
    original.sourceText = "return { version = 1 }";
    ASSERT_TRUE(runtime.LoadScript(original).Succeeded());
    ScriptInstanceHandle instance;
    ASSERT_TRUE(runtime.CreateInstance(original, {}, instance).Succeeded());
    ASSERT_TRUE(runtime.SetField(instance, 42, int32_t{7}));
    backendPtr->events.clear();
    backendPtr->setFields.clear();

    ScriptAsset replacement = original;
    replacement.scriptType = newType.id;
    replacement.sourceText = "return { version = 2 }";
    ASSERT_TRUE(runtime.ReloadAtFrameBoundary(replacement, api, ScriptFrameContext{0.016f, 0.016f, 2.0, 2}).Succeeded());
    EXPECT_EQ(backendPtr->events, (std::vector<std::string>{"OnDisable", "OnEnable"}));
    EXPECT_EQ(backendPtr->setFields, (std::vector<ScriptFieldId>{43}));

    ScriptValue value;
    ASSERT_TRUE(runtime.GetField(instance, 43, value));
    ASSERT_TRUE(std::holds_alternative<int32_t>(value));
    EXPECT_EQ(std::get<int32_t>(value), 7);
}

TEST(ScriptRuntimeTests, FailedReloadReEnablesOldInstanceAndPreservesFields)
{
    ScriptApiDatabase api;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Scripts::FailedReload";
    descriptor.id = MakeStableScriptId(descriptor.name);
    descriptor.fields.push_back({42, "value", {0, ScriptValueKind::Int32, "int32_t", 4, 4, true}});
    ASSERT_TRUE(api.RegisterType(descriptor));

    auto backend = std::make_unique<RecordingBackend>();
    auto* backendPtr = backend.get();
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());

    ScriptAsset original;
    original.assetId = NLS::Core::Assets::AssetId::New();
    original.language = ScriptLanguage::Lua;
    original.scriptType = descriptor.id;
    original.sourceText = "return { version = 1 }";
    ASSERT_TRUE(runtime.LoadScript(original).Succeeded());
    ScriptInstanceHandle instance;
    ASSERT_TRUE(runtime.CreateInstance(original, {}, instance).Succeeded());
    ASSERT_TRUE(runtime.SetField(instance, 42, int32_t{11}));
    backendPtr->events.clear();
    backendPtr->rejectReload = true;

    ScriptAsset replacement = original;
    replacement.sourceText = "return { version = 2 }";
    const auto status = runtime.ReloadAtFrameBoundary(replacement, api, {});
    EXPECT_EQ(status.code, ScriptStatusCode::HotReloadRejected);
    EXPECT_EQ(backendPtr->events, (std::vector<std::string>{"OnDisable", "OnEnable"}));

    ScriptValue value;
    ASSERT_TRUE(runtime.GetField(instance, 42, value));
    ASSERT_TRUE(std::holds_alternative<int32_t>(value));
    EXPECT_EQ(std::get<int32_t>(value), 11);
}

TEST(ScriptRuntimeTests, SchemaMismatchRejectsReloadBeforeLifecycleAndKeepsField)
{
    ScriptApiDatabase api;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Scripts::SchemaReload";
    descriptor.id = MakeStableScriptId(descriptor.name);
    descriptor.fields.push_back({42, "value", {0, ScriptValueKind::Int32, "int32_t", 4, 4, true}});
    ASSERT_TRUE(api.RegisterType(descriptor));

    auto backend = std::make_unique<RecordingBackend>();
    auto* backendPtr = backend.get();
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());

    ScriptAsset original;
    original.assetId = NLS::Core::Assets::AssetId::New();
    original.language = ScriptLanguage::Lua;
    original.scriptType = descriptor.id;
    original.sourceText = "return { version = 1 }";
    ASSERT_TRUE(runtime.LoadScript(original).Succeeded());
    ScriptInstanceHandle instance;
    ASSERT_TRUE(runtime.CreateInstance(original, {}, instance).Succeeded());
    ASSERT_TRUE(runtime.SetField(instance, 42, int32_t{13}));
    backendPtr->events.clear();

    ScriptApiDatabase incompatible = api;
    ScriptTypeDescriptor extra;
    extra.name = "NLS::Scripts::SchemaReloadExtra";
    extra.id = MakeStableScriptId(extra.name);
    ASSERT_TRUE(incompatible.RegisterType(extra));
    ScriptAsset replacement = original;
    replacement.sourceText = "return { version = 2 }";
    const auto status = runtime.ReloadAtFrameBoundary(replacement, incompatible, {});
    EXPECT_EQ(status.code, ScriptStatusCode::SchemaMismatch);
    EXPECT_TRUE(backendPtr->events.empty());

    ScriptValue value;
    ASSERT_TRUE(runtime.GetField(instance, 42, value));
    ASSERT_TRUE(std::holds_alternative<int32_t>(value));
    EXPECT_EQ(std::get<int32_t>(value), 13);
}

TEST(ScriptRuntimeTests, LuaOverloadResolutionUsesDeterministicCosts)
{
    const ScriptType int32Type{0, ScriptValueKind::Int32, "int32_t", 4, 4, true};
    const ScriptType int64Type{0, ScriptValueKind::Int64, "int64_t", 8, 8, true};
    const ScriptType uint64Type{0, ScriptValueKind::UInt64, "uint64_t", 8, 8, true};
    const std::vector<LuaScriptBackend::OverloadCandidate> exact = {
        {11, {int64Type}},
        {12, {int32Type}}};
    const std::vector<ScriptValue> argument = {int32_t{4}};
    ScriptMemberId selected = 0;
    ASSERT_TRUE(LuaScriptBackend::ResolveOverload(exact, argument, selected).Succeeded());
    EXPECT_EQ(selected, 12u);

    const std::vector<LuaScriptBackend::OverloadCandidate> ambiguous = {
        {21, {int64Type}},
        {22, {uint64Type}}};
    EXPECT_EQ(
        LuaScriptBackend::ResolveOverload(ambiguous, argument, selected).code,
        ScriptStatusCode::RuntimeError);
    EXPECT_EQ(selected, 0u);
}

#if NLS_ENABLE_CSHARP_SCRIPTING && NLS_ENABLE_LUA_SCRIPTING && NLS_HAS_LUA_VM
TEST(ScriptRuntimeTests, RuntimeSetupUsesRepositoryLocalSchemaAndDotnetArtifacts)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty()) << "Could not locate the Nullus checkout from the test working directory.";

    ScriptRuntime runtime;
    ScriptRuntimeSetupOptions options;
    options.projectRoot = root / "TestProject";
    const auto setup = InitializeScriptRuntime(runtime, options);

    ASSERT_TRUE(setup.status.Succeeded()) << setup.status.message;
    EXPECT_TRUE(setup.schemaLoaded);
    EXPECT_TRUE(setup.luaRegistered);
    EXPECT_TRUE(setup.csharpRegistered);
    EXPECT_NE(runtime.GetBackend(ScriptLanguage::Lua), nullptr);
    EXPECT_NE(runtime.GetBackend(ScriptLanguage::CSharp), nullptr);
    EXPECT_FALSE(setup.scriptApiDirectory.empty());
    EXPECT_FALSE(setup.dotnetRoot.empty());
    EXPECT_FALSE(setup.managedAssembly.empty());
    EXPECT_TRUE(runtime.IsInitialized());
}

#endif

#if NLS_ENABLE_CSHARP_SCRIPTING

TEST(ScriptRuntimeTests, CoreClrExecutesGeneratedCSharpBehaviourAgainstNativeTransform)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty()) << "Could not locate the Nullus checkout from the test working directory.";

    ScriptApiDatabase api;
    const auto manifestPath = root / "Library" / "ScriptApi" / "ScriptApi.json";
    const auto manifest = ReadTextFile(manifestPath);
    ASSERT_FALSE(manifest.empty()) << manifestPath.string();
    const auto manifestStatus = ScriptApiManifest::FromJson(manifest, api);
    ASSERT_TRUE(manifestStatus.Succeeded()) << manifestStatus.message;
    ASSERT_FALSE(api.GetTypes().empty());

    const auto managedOutput = ResolveManagedOutput(root);
    ASSERT_FALSE(managedOutput.empty());
    const auto runtimeConfig = managedOutput / "GameScripts.runtimeconfig.json";
    const auto assembly = managedOutput / "GameScripts.dll";
    const auto dotnetRoot = ResolveDotnetRoot(root);
    ASSERT_TRUE(std::filesystem::exists(runtimeConfig)) << runtimeConfig.string();
    ASSERT_TRUE(std::filesystem::exists(assembly)) << assembly.string();
    ASSERT_TRUE(std::filesystem::exists(dotnetRoot / "host" / "fxr")) << dotnetRoot.string();

    auto backend = std::make_unique<CoreClrScriptBackend>(ScriptBackendId{41});
    auto* backendPtr = backend.get();
    backendPtr->SetDotnetRoot(dotnetRoot);
    backendPtr->SetHostArtifacts(runtimeConfig, assembly);
    const auto initializeStatus = backendPtr->Initialize(api);
    ASSERT_TRUE(initializeStatus.Succeeded()) << initializeStatus.message;
    ASSERT_FALSE(backendPtr->GetBehaviourManifest().empty());
    const auto transformManifestEntry = std::find_if(
        backendPtr->GetBehaviourManifest().begin(),
        backendPtr->GetBehaviourManifest().end(),
        [](const ManagedBehaviourManifestEntry& entry)
        {
            return entry.simpleName == "TransformMover";
        });
    ASSERT_NE(transformManifestEntry, backendPtr->GetBehaviourManifest().end());
        EXPECT_EQ(transformManifestEntry->typeName, "global::Nullus.EngineScripts.TransformMover");
    EXPECT_TRUE(transformManifestEntry->isPublic);
    EXPECT_TRUE(transformManifestEntry->isConcrete);
        // Generated factories use the parameterless Unity-style constructor
        // and bind the native handle after construction.
        EXPECT_FALSE(transformManifestEntry->hasNativeObjectHandleConstructor);
    EXPECT_TRUE(transformManifestEntry->isComponent);
    const auto manifestComponent = backendPtr->IsComponentAsset("Assets/TransformMover.cs");
    ASSERT_TRUE(manifestComponent.has_value());
    EXPECT_TRUE(*manifestComponent);

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::CSharp;
    asset.sourcePath = "Assets/TransformMover.cs";
    asset.sourceText = ReadTextFile(root / "Managed" / "Nullus.EngineScripts" / "Scripts" / "TransformMover.cs");
    ASSERT_FALSE(asset.sourceText.empty());
    ASSERT_TRUE(backendPtr->LoadScript(asset).Succeeded());

    NLS::Engine::GameObject gameObject("CoreClrNativeObject");
    const auto owner = NativeObjectHandle::FromInstanceId(gameObject.GetInstanceID());
    ScriptInstanceHandle instance;
    const auto createStatus = backendPtr->CreateInstance(asset, owner, instance);
    ASSERT_TRUE(createStatus.Succeeded()) << createStatus.message;

    ASSERT_TRUE(backendPtr->Invoke(instance, ScriptCallback::Awake, {{}, owner}).Succeeded());
    ASSERT_TRUE(backendPtr->Invoke(instance, ScriptCallback::Start, {{}, owner}).Succeeded());

    ScriptValue value;
    const auto speedField = MakeStableScriptId("global::Nullus.EngineScripts.TransformMover::Speed::field");
    ASSERT_TRUE(backendPtr->GetField(instance, speedField, value));
    ASSERT_TRUE(std::holds_alternative<float>(value));
    EXPECT_FLOAT_EQ(std::get<float>(value), 1.0f);
    ASSERT_TRUE(backendPtr->SetField(instance, speedField, 2.0f));

    ScriptInvocationContext update;
    update.frame.deltaTime = 0.5f;
    update.owner = owner;
    ASSERT_TRUE(backendPtr->Invoke(instance, ScriptCallback::Update, update).Succeeded());
    ASSERT_NE(gameObject.GetTransform(), nullptr);
    EXPECT_FLOAT_EQ(gameObject.GetTransform()->GetLocalPosition().x, 1.0f);

    ASSERT_TRUE(backendPtr->GetCapabilities().supportsBatchInvoke);
    const std::array batchInstances{instance};
    ASSERT_TRUE(backendPtr->InvokeBatch(ScriptCallback::Update, batchInstances, update).Succeeded());
    EXPECT_FLOAT_EQ(gameObject.GetTransform()->GetLocalPosition().x, 2.0f);

    const auto awakeCountField = MakeStableScriptId("global::Nullus.EngineScripts.TransformMover::AwakeCount::field");
    const auto startCountField = MakeStableScriptId("global::Nullus.EngineScripts.TransformMover::StartCount::field");
    const auto updateCountField = MakeStableScriptId("global::Nullus.EngineScripts.TransformMover::UpdateCount::field");
    ASSERT_TRUE(backendPtr->GetField(instance, awakeCountField, value));
    EXPECT_EQ(std::get<int32_t>(value), 1);
    ASSERT_TRUE(backendPtr->GetField(instance, startCountField, value));
    EXPECT_EQ(std::get<int32_t>(value), 1);
    ASSERT_TRUE(backendPtr->GetField(instance, updateCountField, value));
    EXPECT_EQ(std::get<int32_t>(value), 2);

    const auto autoSerializedField = MakeStableScriptId("global::Nullus.EngineScripts.TransformMover::AutoSerializedCount::field");
    ASSERT_TRUE(backendPtr->GetField(instance, autoSerializedField, value));
    EXPECT_EQ(std::get<int32_t>(value), 0);
    const auto runtimeOnlyField = MakeStableScriptId("global::Nullus.EngineScripts.TransformMover::RuntimeOnlyCount::field");
    EXPECT_FALSE(backendPtr->GetField(instance, runtimeOnlyField, value));

    ASSERT_TRUE(backendPtr->DestroyInstance(instance).Succeeded());
    EXPECT_EQ(backendPtr->Invoke(instance, ScriptCallback::Update, {}).code, ScriptStatusCode::InvalidHandle);
    EXPECT_TRUE(backendPtr->DestroyInstance(instance).Succeeded());
    backendPtr->Shutdown();
}

TEST(ScriptRuntimeTests, CoreClrInvokesPrivateCSharpLifecycleMethods)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty());

    ScriptApiDatabase api;
    const auto manifest = ReadTextFile(root / "Library" / "ScriptApi" / "ScriptApi.json");
    ASSERT_TRUE(ScriptApiManifest::FromJson(manifest, api).Succeeded());
    const auto managedOutput = ResolveManagedOutput(root);
    ASSERT_FALSE(managedOutput.empty());
    const auto dotnetRoot = ResolveDotnetRoot(root);

    auto backend = std::make_unique<CoreClrScriptBackend>(ScriptBackendId{42});
    auto* backendPtr = backend.get();
    backendPtr->SetDotnetRoot(dotnetRoot);
    backendPtr->SetHostArtifacts(
        managedOutput / "GameScripts.runtimeconfig.json",
        managedOutput / "GameScripts.dll");
    ASSERT_TRUE(backendPtr->Initialize(api).Succeeded());

    const auto entry = std::find_if(
        backendPtr->GetBehaviourManifest().begin(),
        backendPtr->GetBehaviourManifest().end(),
        [](const ManagedBehaviourManifestEntry& item) { return item.simpleName == "PrivateLifecycle"; });
    ASSERT_NE(entry, backendPtr->GetBehaviourManifest().end());
    EXPECT_FALSE(entry->hasNativeObjectHandleConstructor);
    EXPECT_TRUE(entry->isComponent);

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::CSharp;
    asset.sourcePath = "Assets/PrivateLifecycle.cs";
    asset.sourceText = ReadTextFile(root / "Managed" / "Nullus.EngineScripts" / "Scripts" / "PrivateLifecycle.cs");
    ASSERT_FALSE(asset.sourceText.empty());
    ASSERT_TRUE(backendPtr->LoadScript(asset).Succeeded());

    NLS::Engine::GameObject gameObject("PrivateLifecycleOwner");
    const auto owner = NativeObjectHandle::FromInstanceId(gameObject.GetInstanceID());
    ScriptInstanceHandle instance;
    ASSERT_TRUE(backendPtr->CreateInstance(asset, owner, instance).Succeeded());
    for (const auto callback : {ScriptCallback::Awake, ScriptCallback::Start, ScriptCallback::OnEnable,
                                ScriptCallback::OnDisable, ScriptCallback::Update, ScriptCallback::FixedUpdate,
                                ScriptCallback::LateUpdate, ScriptCallback::OnDestroy})
    {
        ScriptInvocationContext invocation;
        invocation.owner = owner;
        ASSERT_TRUE(backendPtr->Invoke(instance, callback, invocation).Succeeded());
    }

    const auto field = [&](std::string_view name)
    {
        return MakeStableScriptId("global::Nullus.EngineScripts.PrivateLifecycle::" + std::string(name) + "::field");
    };
    ScriptValue value;
    for (const auto name : {"AwakeCount", "StartCount", "EnableCount", "DisableCount", "UpdateCount",
                            "FixedUpdateCount", "LateUpdateCount", "DestroyCount"})
    {
        ASSERT_TRUE(backendPtr->GetField(instance, field(name), value));
        ASSERT_TRUE(std::holds_alternative<int32_t>(value));
        EXPECT_EQ(std::get<int32_t>(value), 1);
    }

    EXPECT_TRUE(backendPtr->DestroyInstance(instance).Succeeded());
    backendPtr->Shutdown();
}

TEST(ScriptRuntimeTests, CoreClrRotatingCubeScriptCreatesAndRotatesScenePrimitive)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty());

    ScriptApiDatabase api;
    const auto manifest = ReadTextFile(root / "Library" / "ScriptApi" / "ScriptApi.json");
    ASSERT_TRUE(ScriptApiManifest::FromJson(manifest, api).Succeeded());

    const auto managedOutput = ResolveManagedOutput(root);
    ASSERT_FALSE(managedOutput.empty());
    const auto dotnetRoot = ResolveDotnetRoot(root);
    auto backend = std::make_unique<CoreClrScriptBackend>(ScriptBackendId{45});
    auto* backendPtr = backend.get();
    backendPtr->SetDotnetRoot(dotnetRoot);
    backendPtr->SetHostArtifacts(
        managedOutput / "GameScripts.runtimeconfig.json",
        managedOutput / "GameScripts.dll");
    ASSERT_TRUE(backendPtr->Initialize(api).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::CSharp;
    asset.sourcePath = "Assets/RotatingCube.cs";
    asset.scriptType = MakeStableScriptId("CSharp:" + asset.sourcePath);
    asset.sourceText = ReadTextFile(root / "TestProject" / "Assets" / "RotatingCube.cs");
    ASSERT_FALSE(asset.sourceText.empty());
    const auto* descriptor = backendPtr->FindScriptType(asset.scriptType);
    ASSERT_NE(descriptor, nullptr);
    const auto angleField = std::find_if(
        descriptor->fields.begin(),
        descriptor->fields.end(),
        [](const auto& field) { return field.name == "_angle"; });
    ASSERT_NE(angleField, descriptor->fields.end());
    EXPECT_TRUE(angleField->serialized);
    EXPECT_EQ(angleField->type.kind, ScriptValueKind::Float);
    ASSERT_TRUE(backendPtr->LoadScript(asset).Succeeded());

    NLS::Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("RotatingCubeRoot");
    const auto owner = NativeObjectHandle::FromInstanceId(parent.GetInstanceID());
    ScriptInstanceHandle instance;
    ASSERT_TRUE(backendPtr->CreateInstance(asset, owner, instance).Succeeded());
    ASSERT_TRUE(backendPtr->Invoke(instance, ScriptCallback::Awake, {{}, owner}).Succeeded());

    auto* cube = scene.FindGameObjectByName("Cube");
    ASSERT_NE(cube, nullptr);
    ASSERT_TRUE(backendPtr->Invoke(instance, ScriptCallback::Update, {{0.5f, 0.5f, 0.5, 1}, owner}).Succeeded());
    ASSERT_NE(cube->GetTransform(), nullptr);
    EXPECT_NE(cube->GetTransform()->GetLocalRotation().y, 0.0f);

    ASSERT_TRUE(backendPtr->DestroyInstance(instance).Succeeded());
    backendPtr->Shutdown();
}

TEST(ScriptRuntimeTests, CoreClrExceptionReportsPortablePdbSourceLocation)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty()) << "Could not locate the Nullus checkout from the test working directory.";

    ScriptApiDatabase api;
    const auto manifest = ReadTextFile(root / "Library" / "ScriptApi" / "ScriptApi.json");
    ASSERT_TRUE(ScriptApiManifest::FromJson(manifest, api).Succeeded());

    const auto managedOutput = ResolveManagedOutput(root);
    ASSERT_FALSE(managedOutput.empty());
    const auto runtimeConfig = managedOutput / "GameScripts.runtimeconfig.json";
    const auto assembly = managedOutput / "GameScripts.dll";
    const auto dotnetRoot = ResolveDotnetRoot(root);
    ASSERT_TRUE(std::filesystem::exists(runtimeConfig));
    ASSERT_TRUE(std::filesystem::exists(assembly));

    auto backend = std::make_unique<CoreClrScriptBackend>(ScriptBackendId{44});
    auto* backendPtr = backend.get();
    backendPtr->SetDotnetRoot(dotnetRoot);
    backendPtr->SetHostArtifacts(runtimeConfig, assembly);
    ASSERT_TRUE(backendPtr->Initialize(api).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::CSharp;
    asset.sourcePath = "Assets/TransformMover.cs";
    asset.sourceText = ReadTextFile(root / "Managed" / "Nullus.EngineScripts" / "Scripts" / "TransformMover.cs");
    ASSERT_FALSE(asset.sourceText.empty());
    ASSERT_TRUE(backendPtr->LoadScript(asset).Succeeded());

    NLS::Engine::GameObject gameObject("CoreClrDiagnostic");
    const auto owner = NativeObjectHandle::FromInstanceId(gameObject.GetInstanceID());
    ScriptInstanceHandle instance;
    ASSERT_TRUE(backendPtr->CreateInstance(asset, owner, instance).Succeeded());
    const auto throwField = MakeStableScriptId("global::Nullus.EngineScripts.TransformMover::ThrowOnUpdate::field");
    ASSERT_TRUE(backendPtr->SetField(instance, throwField, true));

    const auto status = backendPtr->Invoke(instance, ScriptCallback::Update, {{}, owner});
    EXPECT_EQ(status.code, ScriptStatusCode::Exception);
    const auto diagnostic = backendPtr->ConsumeLastDiagnostic();
    ASSERT_TRUE(diagnostic.has_value());
    EXPECT_EQ(diagnostic->language, ScriptLanguage::CSharp);
    EXPECT_NE(diagnostic->message.find("TransformMover debug exception"), std::string::npos);
    EXPECT_NE(diagnostic->stackTrace.find("TransformMover"), std::string::npos);
    EXPECT_GT(diagnostic->line, 0);
    EXPECT_NE(diagnostic->sourcePath.find("TransformMover.cs"), std::string::npos);
    EXPECT_FALSE(backendPtr->ConsumeLastDiagnostic().has_value());
    EXPECT_TRUE(backendPtr->DestroyInstance(instance).Succeeded());
    backendPtr->Shutdown();
}

TEST(ScriptRuntimeTests, CoreClrHotReloadPreservesFieldsRollsBackAndCollectsOldAlc)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty()) << "Could not locate the Nullus checkout from the test working directory.";

    ScriptApiDatabase api;
    const auto manifestPath = root / "Library" / "ScriptApi" / "ScriptApi.json";
    const auto manifest = ReadTextFile(manifestPath);
    ASSERT_FALSE(manifest.empty()) << manifestPath.string();
    ASSERT_TRUE(ScriptApiManifest::FromJson(manifest, api).Succeeded());

    const auto managedOutput = ResolveManagedOutput(root);
    ASSERT_FALSE(managedOutput.empty());
    const auto runtimeConfig = managedOutput / "GameScripts.runtimeconfig.json";
    const auto assembly = managedOutput / "GameScripts.dll";
    const auto dotnetRoot = ResolveDotnetRoot(root);
    ASSERT_TRUE(std::filesystem::exists(runtimeConfig)) << runtimeConfig.string();
    ASSERT_TRUE(std::filesystem::exists(assembly)) << assembly.string();
    ASSERT_TRUE(std::filesystem::exists(dotnetRoot / "host" / "fxr")) << dotnetRoot.string();

    auto backend = std::make_unique<CoreClrScriptBackend>(ScriptBackendId{43});
    auto* backendPtr = backend.get();
    backendPtr->SetDotnetRoot(dotnetRoot);
    backendPtr->SetHostArtifacts(runtimeConfig, assembly);
    ASSERT_TRUE(backendPtr->Initialize(api).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::CSharp;
    asset.sourcePath = "Assets/TransformMover.cs";
    asset.sourceText = ReadTextFile(root / "Managed" / "Nullus.EngineScripts" / "Scripts" / "TransformMover.cs");
    ASSERT_FALSE(asset.sourceText.empty());

    EXPECT_EQ(backendPtr->Reload(asset.assetId, api).code, ScriptStatusCode::AssetNotLoaded);
    ASSERT_TRUE(backendPtr->LoadScript(asset).Succeeded());

    NLS::Engine::GameObject gameObject("CoreClrHotReload");
    const auto owner = NativeObjectHandle::FromInstanceId(gameObject.GetInstanceID());
    ScriptInstanceHandle instance;
    ASSERT_TRUE(backendPtr->CreateInstance(asset, owner, instance).Succeeded());
    ASSERT_TRUE(backendPtr->Invoke(instance, ScriptCallback::Awake, {{}, owner}).Succeeded());
    ASSERT_TRUE(backendPtr->Invoke(instance, ScriptCallback::Start, {{}, owner}).Succeeded());

    const auto speedField = MakeStableScriptId("global::Nullus.EngineScripts.TransformMover::Speed::field");
    const auto updateCountField = MakeStableScriptId("global::Nullus.EngineScripts.TransformMover::UpdateCount::field");
    ASSERT_TRUE(backendPtr->SetField(instance, speedField, 2.0f));

    ScriptInvocationContext update;
    update.frame.deltaTime = 0.5f;
    update.owner = owner;
    ASSERT_TRUE(backendPtr->Invoke(instance, ScriptCallback::Update, update).Succeeded());
    EXPECT_FLOAT_EQ(gameObject.GetTransform()->GetLocalPosition().x, 1.0f);

    ScriptValue value;
    ASSERT_TRUE(backendPtr->GetField(instance, updateCountField, value));
    ASSERT_TRUE(std::holds_alternative<int32_t>(value));
    EXPECT_EQ(std::get<int32_t>(value), 1);

    void* diagnosticFunction = nullptr;
    const auto diagnosticStatus = backendPtr->GetHost().GetUnmanagedFunctionPointer(
        assembly,
        "Nullus.GameScripts.GameScriptsExports, GameScripts",
        "CollectAndGetLiveProjectLoadContextCount",
        diagnosticFunction);
    ASSERT_TRUE(diagnosticStatus.Succeeded()) << diagnosticStatus.message;
    using CollectLoadContextCountFn = int (*)();
    const auto collectLoadContextCount = reinterpret_cast<CollectLoadContextCountFn>(diagnosticFunction);
    ASSERT_NE(collectLoadContextCount, nullptr);
    EXPECT_EQ(collectLoadContextCount(), 0);

    for (int reloadIndex = 0; reloadIndex < 3; ++reloadIndex)
    {
        const auto reloadStatus = backendPtr->Reload(asset.assetId, api);
        ASSERT_TRUE(reloadStatus.Succeeded()) << reloadStatus.message;
        ASSERT_TRUE(backendPtr->GetField(instance, speedField, value));
        ASSERT_TRUE(std::holds_alternative<float>(value));
        EXPECT_FLOAT_EQ(std::get<float>(value), 2.0f);
        ASSERT_TRUE(backendPtr->GetField(instance, updateCountField, value));
        ASSERT_TRUE(std::holds_alternative<int32_t>(value));
        EXPECT_EQ(std::get<int32_t>(value), 1);
        EXPECT_EQ(collectLoadContextCount(), 1);
    }

    // A malformed replacement must leave the last committed registration and
    // instance usable.  The file is intentionally valid enough to pass the
    // existence check but cannot be loaded as a managed assembly.
    const auto invalidAssembly = std::filesystem::temp_directory_path()
        / ("Nullus.InvalidGameScripts." + asset.assetId.ToString() + ".dll");
    {
        std::ofstream output(invalidAssembly, std::ios::binary);
        ASSERT_TRUE(output.good());
        output << "not a managed assembly";
    }
    backendPtr->SetHostArtifacts(runtimeConfig, invalidAssembly);
    const auto failedReload = backendPtr->Reload(asset.assetId, api);
    EXPECT_EQ(failedReload.code, ScriptStatusCode::HotReloadRejected);
    backendPtr->SetHostArtifacts(runtimeConfig, assembly);
    std::error_code removeError;
    std::filesystem::remove(invalidAssembly, removeError);
    EXPECT_FALSE(removeError);

    ASSERT_TRUE(backendPtr->Invoke(instance, ScriptCallback::Update, update).Succeeded());
    EXPECT_FLOAT_EQ(gameObject.GetTransform()->GetLocalPosition().x, 2.0f);
    ASSERT_TRUE(backendPtr->GetField(instance, speedField, value));
    EXPECT_FLOAT_EQ(std::get<float>(value), 2.0f);
    ASSERT_TRUE(backendPtr->GetField(instance, updateCountField, value));
    EXPECT_EQ(std::get<int32_t>(value), 2);

    ASSERT_TRUE(backendPtr->DestroyInstance(instance).Succeeded());
    backendPtr->Shutdown();
}

TEST(ScriptRuntimeTests, CoreClrBatchInvocationReducesManagedAbiBoundaries)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty()) << "Could not locate the Nullus checkout from the test working directory.";

    ScriptApiDatabase api;
    const auto manifestPath = root / "Library" / "ScriptApi" / "ScriptApi.json";
    const auto manifest = ReadTextFile(manifestPath);
    ASSERT_FALSE(manifest.empty()) << manifestPath.string();
    ASSERT_TRUE(ScriptApiManifest::FromJson(manifest, api).Succeeded());

    const auto managedOutput = ResolveManagedOutput(root);
    ASSERT_FALSE(managedOutput.empty());
    const auto runtimeConfig = managedOutput / "GameScripts.runtimeconfig.json";
    const auto assembly = managedOutput / "GameScripts.dll";
    const auto dotnetRoot = ResolveDotnetRoot(root);
    ASSERT_TRUE(std::filesystem::exists(runtimeConfig)) << runtimeConfig.string();
    ASSERT_TRUE(std::filesystem::exists(assembly)) << assembly.string();
    ASSERT_TRUE(std::filesystem::exists(dotnetRoot / "host" / "fxr")) << dotnetRoot.string();

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::CSharp;
    asset.sourcePath = "Assets/TransformMover.cs";
    asset.sourceText = ReadTextFile(root / "Managed" / "Nullus.EngineScripts" / "Scripts" / "TransformMover.cs");
    ASSERT_FALSE(asset.sourceText.empty());

    NLS::Engine::GameObject gameObject("CoreClrBatchBenchmark");
    const auto owner = NativeObjectHandle::FromInstanceId(gameObject.GetInstanceID());
    ScriptInvocationContext update;
    update.frame.deltaTime = 0.0f;
    update.owner = owner;

    for (const size_t scriptCount : {size_t{1}, size_t{1000}, size_t{10000}})
    {
        auto backend = std::make_unique<CoreClrScriptBackend>(ScriptBackendId{
            static_cast<uint16_t>(50 + (scriptCount == 1000 ? 1 : scriptCount == 10000 ? 2 : 0))});
        auto* backendPtr = backend.get();
        backendPtr->SetDotnetRoot(dotnetRoot);
        backendPtr->SetHostArtifacts(runtimeConfig, assembly);
        ASSERT_TRUE(backendPtr->Initialize(api).Succeeded());
        ASSERT_TRUE(backendPtr->LoadScript(asset).Succeeded());

        std::vector<ScriptInstanceHandle> instances;
        instances.reserve(scriptCount);
        for (size_t index = 0; index < scriptCount; ++index)
        {
            ScriptInstanceHandle instance;
            ASSERT_TRUE(backendPtr->CreateInstance(asset, owner, instance).Succeeded());
            instances.push_back(instance);
        }

        backendPtr->ClearInvocationTrace();
        const auto batchStart = std::chrono::steady_clock::now();
        ASSERT_TRUE(backendPtr->InvokeBatch(ScriptCallback::Update, instances, update).Succeeded());
        const auto batchElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - batchStart).count();
        RecordProperty(("coreclr_batch_us_" + std::to_string(scriptCount)).c_str(), batchElapsed);
        EXPECT_EQ(backendPtr->GetBatchCallCount(), 1u);
        EXPECT_EQ(backendPtr->GetBatchInstanceCount(), scriptCount);
        EXPECT_EQ(backendPtr->GetInvokeCallCount(), 0u);
        const auto batchAbiCalls = backendPtr->GetBatchCallCount();

        backendPtr->ClearInvocationTrace();
        const auto directStart = std::chrono::steady_clock::now();
        for (const auto instance : instances)
            ASSERT_TRUE(backendPtr->Invoke(instance, ScriptCallback::Update, update).Succeeded());
        const auto directElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - directStart).count();
        RecordProperty(("coreclr_direct_us_" + std::to_string(scriptCount)).c_str(), directElapsed);
        EXPECT_EQ(backendPtr->GetBatchCallCount(), 0u);
        EXPECT_EQ(backendPtr->GetBatchInstanceCount(), 0u);
        EXPECT_EQ(backendPtr->GetInvokeCallCount(), scriptCount);
        const auto directAbiCalls = backendPtr->GetInvokeCallCount();
        if (scriptCount >= 1000)
            EXPECT_LE(batchAbiCalls, directAbiCalls / 10u);

        for (const auto instance : instances)
            ASSERT_TRUE(backendPtr->DestroyInstance(instance).Succeeded());
        backendPtr->Shutdown();
    }
}
#endif

#if NLS_ENABLE_CSHARP_SCRIPTING && NLS_HAS_LUA_VM
TEST(ScriptRuntimeTests, CSharpAndLuaComponentsShareOwnerLifecycleAndIsolateErrors)
{
    const auto root = FindNullusRoot();
    ASSERT_FALSE(root.empty()) << "Could not locate the Nullus checkout from the test working directory.";

    ScriptApiDatabase api;
    const auto manifestPath = root / "Library" / "ScriptApi" / "ScriptApi.json";
    const auto manifest = ReadTextFile(manifestPath);
    ASSERT_FALSE(manifest.empty()) << manifestPath.string();
    ASSERT_TRUE(ScriptApiManifest::FromJson(manifest, api).Succeeded());

    const auto managedOutput = ResolveManagedOutput(root);
    ASSERT_FALSE(managedOutput.empty());
    const auto runtimeConfig = managedOutput / "GameScripts.runtimeconfig.json";
    const auto assembly = managedOutput / "GameScripts.dll";
    const auto dotnetRoot = ResolveDotnetRoot(root);
    ASSERT_TRUE(std::filesystem::exists(runtimeConfig)) << runtimeConfig.string();
    ASSERT_TRUE(std::filesystem::exists(assembly)) << assembly.string();
    ASSERT_TRUE(std::filesystem::exists(dotnetRoot / "host" / "fxr")) << dotnetRoot.string();

    auto csharp = std::make_unique<CoreClrScriptBackend>(ScriptBackendId{41});
    csharp->SetDotnetRoot(dotnetRoot);
    csharp->SetHostArtifacts(runtimeConfig, assembly);
    auto lua = std::make_unique<LuaScriptBackend>(ScriptBackendId{42});

    ScriptRuntime runtime;
    ScriptErrorConsole errorConsole;
    errorConsole.Attach(runtime);
    ASSERT_TRUE(runtime.RegisterBackend(std::move(csharp)).Succeeded());
    ASSERT_TRUE(runtime.RegisterBackend(std::move(lua)).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());

    ScriptAsset csharpAsset;
    csharpAsset.assetId = NLS::Core::Assets::AssetId::New();
    csharpAsset.language = ScriptLanguage::CSharp;
    // The repository sample lives in Managed/Nullus.EngineScripts/Scripts, so
    // its generated manifest uses the canonical file-name fallback.  Real
    // project assets under Assets/ retain their full Assets/... path.
    csharpAsset.sourcePath = "TransformMover.cs";
    csharpAsset.scriptType = MakeStableScriptId("CSharp:" + csharpAsset.sourcePath);
    csharpAsset.sourceText = ReadTextFile(root / "Managed" / "Nullus.EngineScripts" / "Scripts" / "TransformMover.cs");
    ASSERT_FALSE(csharpAsset.sourceText.empty());

    const auto* csharpDescriptor = runtime.FindScriptType(csharpAsset);
    ASSERT_NE(csharpDescriptor, nullptr);
    const auto publicField = std::find_if(
        csharpDescriptor->fields.begin(),
        csharpDescriptor->fields.end(),
        [](const auto& field) { return field.name == "AutoSerializedCount"; });
    ASSERT_NE(publicField, csharpDescriptor->fields.end());
    EXPECT_TRUE(publicField->serialized);
    EXPECT_EQ(publicField->type.kind, ScriptValueKind::Int32);

    ScriptAsset luaAsset;
    luaAsset.assetId = NLS::Core::Assets::AssetId::New();
    luaAsset.language = ScriptLanguage::Lua;
    luaAsset.sourcePath = "Assets/SharedMover.lua";
    luaAsset.sourceText =
        "return { "
        "Awake = function(self) self.gameObject.transform.localPosition = { x = 0, y = 1, z = 0 } end, "
        "OnEnable = function(self) self.gameObject.transform.localPosition = { x = 0, y = 2, z = 0 } end, "
        "Start = function(self) self.gameObject.transform.localPosition = { x = 0, y = 3, z = 0 } end, "
         "Update = function(self) "
        "local p = self.gameObject.transform.localPosition; "
        "self.gameObject.transform.localPosition = { x = p.x, y = 2, z = 3 }; "
        "error('lua update failure') end }";

    NLS::Engine::GameObject gameObject("CSharpLuaSharedOwner");
    auto* csharpComponent = gameObject.AddComponent<ScriptComponent>([&](auto* component)
    {
        auto* script = static_cast<ScriptComponent*>(component);
        script->SetRuntime(&runtime);
        script->SetScriptAsset(csharpAsset);
    });
    auto* luaComponent = gameObject.AddComponent<ScriptComponent>([&](auto* component)
    {
        auto* script = static_cast<ScriptComponent*>(component);
        script->SetRuntime(&runtime);
        script->SetScriptAsset(luaAsset);
    });
    ASSERT_NE(csharpComponent, nullptr);
    ASSERT_NE(luaComponent, nullptr);

    // This is the same descriptor path consumed by the Editor Inspector.
    // Public C# instance fields must be visible without an explicit
    // [SerializeField] attribute.
    SerializedObject inspectorObject(*csharpComponent);
    ASSERT_TRUE(inspectorObject.IsValid());
    ASSERT_NE(inspectorObject.FindProperty("AutoSerializedCount"), nullptr);
    ASSERT_NE(inspectorObject.FindProperty("Speed"), nullptr);

    const auto owner = NativeObjectHandle::FromInstanceId(gameObject.GetInstanceID());
    EXPECT_EQ(NativeObjectHandle::FromInstanceId(csharpComponent->gameobject()->GetInstanceID()), owner);
    EXPECT_EQ(NativeObjectHandle::FromInstanceId(luaComponent->gameobject()->GetInstanceID()), owner);

    // GameObject dispatch is component-order stable: C# was added first and
    // Lua second, so both observe the same native owner in a deterministic order.
    gameObject.OnAwake();
    ASSERT_TRUE(csharpComponent->GetInstance().IsValid());
    ASSERT_TRUE(luaComponent->GetInstance().IsValid());
    ASSERT_EQ(csharpComponent->GetInstance().backendId, 41u);
    ASSERT_EQ(luaComponent->GetInstance().backendId, 42u);
    ASSERT_NE(gameObject.GetTransform(), nullptr);
    EXPECT_FLOAT_EQ(gameObject.GetTransform()->GetLocalPosition().y, 1.0f);

    gameObject.OnEnable();
    EXPECT_FLOAT_EQ(gameObject.GetTransform()->GetLocalPosition().y, 2.0f);
    gameObject.OnStart();
    EXPECT_FLOAT_EQ(gameObject.GetTransform()->GetLocalPosition().y, 3.0f);

    // C# moves x first; Lua reads that shared Transform, writes y/z, then
    // fails.  The failure is reported but does not prevent the C# component
    // from running or roll back the Lua native write.
    ScriptFrameContext updateFrame;
    updateFrame.deltaTime = 0.5f;
    updateFrame.unscaledDeltaTime = 0.5f;
    ASSERT_TRUE(runtime.BeginScheduledFrame(ScriptCallback::Update, updateFrame).Succeeded());
    gameObject.OnUpdate(0.5f);
    EXPECT_FLOAT_EQ(gameObject.GetTransform()->GetLocalPosition().x, 0.0f);
    const auto scheduledStatus = runtime.FlushScheduledFrame();
    EXPECT_EQ(scheduledStatus.code, ScriptStatusCode::RuntimeError);
    const auto position = gameObject.GetTransform()->GetLocalPosition();
    EXPECT_FLOAT_EQ(position.x, 0.5f);
    EXPECT_FLOAT_EQ(position.y, 2.0f);
    EXPECT_FLOAT_EQ(position.z, 3.0f);

    ScriptValue value;
    const auto updateCountField = MakeStableScriptId("global::Nullus.EngineScripts.TransformMover::UpdateCount::field");
    ASSERT_TRUE(runtime.GetField(csharpComponent->GetInstance(), updateCountField, value));
    ASSERT_TRUE(std::holds_alternative<int32_t>(value));
    EXPECT_EQ(std::get<int32_t>(value), 1);
    ASSERT_EQ(runtime.GetErrors().size(), 1u);
    EXPECT_EQ(runtime.GetErrors().front().language, ScriptLanguage::Lua);
    EXPECT_NE(runtime.GetErrors().front().sourcePath, "");
    EXPECT_NE(runtime.GetErrors().front().message.find("lua update failure"), std::string::npos);
    ASSERT_EQ(errorConsole.GetErrors().size(), 1u);

    // Destroying one language instance does not invalidate the other one.
    luaComponent->OnDestroy();
    EXPECT_FALSE(luaComponent->GetInstance().IsValid());
    ASSERT_TRUE(csharpComponent->GetInstance().IsValid());
    runtime.ClearErrors();
    ASSERT_TRUE(runtime.BeginScheduledFrame(ScriptCallback::Update, updateFrame).Succeeded());
    gameObject.OnUpdate(0.5f);
    ASSERT_TRUE(runtime.FlushScheduledFrame().Succeeded());
    EXPECT_TRUE(runtime.GetErrors().empty());
    EXPECT_FLOAT_EQ(gameObject.GetTransform()->GetLocalPosition().x, 1.0f);

    gameObject.OnDisable();
    gameObject.OnDestroy();
    EXPECT_FALSE(csharpComponent->GetInstance().IsValid());
}
#endif

TEST(ScriptRuntimeTests, LuaSourceValidationReportsAssetLocation)
{
    LuaScriptBackend backend;
    ASSERT_TRUE(backend.Initialize({}).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    asset.sourcePath = "Assets/Player.lua";
    asset.sourceText = "return { Update = function(self, dt) end }";
    ASSERT_TRUE(backend.LoadScript(asset).Succeeded());

    asset.sourceText = "return { io = os }";
    const auto status = backend.LoadScript(asset);
    EXPECT_EQ(status.code, ScriptStatusCode::CompilationFailed);
    EXPECT_NE(status.message.find("Assets/Player.lua:1"), std::string::npos);

    asset.sourceText = "-- os/debug/io are words in this comment\nreturn { message = 'os' }";
    EXPECT_TRUE(backend.LoadScript(asset).Succeeded());
    backend.Shutdown();
}

#if NLS_HAS_LUA_VM
TEST(ScriptRuntimeTests, LuaPandaUsesPrivateEnvironmentAndLeavesGameplaySandboxClosed)
{
#if NLS_HAS_LUAPANDA_SOCKET
    LuaScriptBackend backend;
    backend.SetLuaPandaDebugging(true);
    const auto debugStatus = backend.Initialize({});
    ASSERT_TRUE(debugStatus.Succeeded()) << debugStatus.message;
    EXPECT_TRUE(backend.IsLuaPandaDebuggerActive());
    EXPECT_EQ(backend.GetLuaPandaConnectionState(), LuaPandaConnectionState::WaitingForAttach);
    EXPECT_FALSE(backend.IsLuaPandaConnected());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    asset.sourcePath = "Assets/Sandbox.lua";
    asset.sourceText =
        "return { Update = function(self, dt) "
        "local missing = {'debug', 'io', 'os', 'package'} "
        "for _, name in ipairs(missing) do assert(_G[name] == nil) end "
        "end }";
    const auto sandboxLoadStatus = backend.LoadScript(asset);
    ASSERT_TRUE(sandboxLoadStatus.Succeeded()) << sandboxLoadStatus.message;
    ScriptInstanceHandle instance;
    ASSERT_TRUE(backend.CreateInstance(asset, {}, instance).Succeeded());
    EXPECT_TRUE(backend.Invoke(instance, ScriptCallback::Update, {}).Succeeded());
    backend.Shutdown();
    EXPECT_EQ(backend.GetLuaPandaConnectionState(), LuaPandaConnectionState::Disabled);
#else
    GTEST_SKIP() << "LuaPanda socket target is disabled.";
#endif
}

TEST(ScriptRuntimeTests, LuaRuntimeExecutesModuleLifecycleAndPortableFields)
{
    ScriptApiDatabase api;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Scripts::Mover";
    descriptor.id = MakeStableScriptId(descriptor.name);
    descriptor.fields.push_back({42, "speed", {0, ScriptValueKind::Float, "float", 4, 4, true}});
    ASSERT_TRUE(api.RegisterType(descriptor));

    LuaScriptBackend backend;
    ASSERT_TRUE(backend.Initialize(api).Succeeded());
    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    asset.scriptType = descriptor.id;
    asset.sourcePath = "Assets/Mover.lua";
    asset.sourceText = "return { speed = 1.0, Update = function(self) self.speed = self.speed + Nullus.Time.deltaTime end }";
    ASSERT_TRUE(backend.LoadScript(asset).Succeeded());

    ScriptInstanceHandle instance;
    ASSERT_TRUE(backend.CreateInstance(asset, NativeObjectHandle::FromInstanceId(10), instance).Succeeded());
    ScriptInvocationContext invocation;
    invocation.frame.deltaTime = 0.5f;
    ASSERT_TRUE(backend.Invoke(instance, ScriptCallback::Update, invocation).Succeeded());

    ScriptValue value;
    ASSERT_TRUE(backend.GetField(instance, 42, value));
    ASSERT_TRUE(std::holds_alternative<float>(value));
    EXPECT_FLOAT_EQ(std::get<float>(value), 1.5f);

    ASSERT_TRUE(backend.SetField(instance, 42, 2.0f));
    ASSERT_TRUE(backend.GetField(instance, 42, value));
    EXPECT_FLOAT_EQ(std::get<float>(value), 2.0f);
    EXPECT_TRUE(backend.DestroyInstance(instance).Succeeded());
    backend.Shutdown();
}

TEST(ScriptRuntimeTests, LuaNativeUserdataBindsGameObjectAndTransform)
{
    LuaScriptBackend backend;
    ASSERT_TRUE(backend.Initialize({}).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    asset.sourcePath = "Assets/NativeMover.lua";
    asset.sourceText =
        "return { Update = function(self) "
        "self.gameObject.transform.localPosition = { x = Nullus.Time.deltaTime, y = 2, z = 3 }; "
        "self.gameObject.active = false end }";
    ASSERT_TRUE(backend.LoadScript(asset).Succeeded());

    NLS::Engine::GameObject gameObject("LuaNativeObject");
    ScriptInstanceHandle instance;
    ASSERT_TRUE(backend.CreateInstance(
        asset,
        NativeObjectHandle::FromInstanceId(gameObject.GetInstanceID()),
        instance)
                    .Succeeded());
    ScriptInvocationContext invocation;
    invocation.frame.deltaTime = 0.25f;
    ASSERT_TRUE(backend.Invoke(instance, ScriptCallback::Update, invocation).Succeeded());
    EXPECT_FALSE(gameObject.GetActive());
    ASSERT_NE(gameObject.GetTransform(), nullptr);
    const auto position = gameObject.GetTransform()->GetLocalPosition();
    EXPECT_FLOAT_EQ(position.x, 0.25f);
    EXPECT_FLOAT_EQ(position.y, 2.0f);
    EXPECT_FLOAT_EQ(position.z, 3.0f);

    EXPECT_TRUE(backend.DestroyInstance(instance).Succeeded());
    backend.Shutdown();
}

TEST(ScriptRuntimeTests, LuaNativeUserdataBindsCameraAndLightProperties)
{
    LuaScriptBackend backend;
    ASSERT_TRUE(backend.Initialize({}).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    asset.sourcePath = "Assets/NativeRendering.lua";
    asset.sourceText =
        "return { Update = function(self) "
        "local camera = self.gameObject:GetComponent('Camera'); "
        "camera.fieldOfView = 72; camera.orthographicSize = 8; "
        "camera.nearClipPlane = 0.25; camera.farClipPlane = 250; "
        "camera.clearColor = { x = 0.1, y = 0.2, z = 0.3 }; camera.orthographic = true; "
        "local light = self.gameObject:GetComponent('Light'); "
        "light.color = { x = 0.2, y = 0.4, z = 0.6 }; light.intensity = 3; "
        "light.range = 12; light.spotAngle = 35; light.type = 2 end }";
    ASSERT_TRUE(backend.LoadScript(asset).Succeeded());

    NLS::Engine::GameObject gameObject("LuaRenderingObject");
    auto* camera = gameObject.AddComponent<NLS::Engine::Components::CameraComponent>();
    auto* light = gameObject.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(camera, nullptr);
    ASSERT_NE(light, nullptr);

    ScriptInstanceHandle instance;
    ASSERT_TRUE(backend.CreateInstance(
        asset,
        NativeObjectHandle::FromInstanceId(gameObject.GetInstanceID()),
        instance)
                    .Succeeded());
    ASSERT_TRUE(backend.Invoke(instance, ScriptCallback::Update, {}).Succeeded());

    EXPECT_FLOAT_EQ(camera->GetFov(), 72.0f);
    EXPECT_FLOAT_EQ(camera->GetSize(), 8.0f);
    EXPECT_FLOAT_EQ(camera->GetNear(), 0.25f);
    EXPECT_FLOAT_EQ(camera->GetFar(), 250.0f);
    EXPECT_EQ(camera->GetProjectionMode(), NLS::Render::Settings::EProjectionMode::ORTHOGRAPHIC);
    EXPECT_FLOAT_EQ(camera->GetClearColor().x, 0.1f);
    EXPECT_FLOAT_EQ(camera->GetClearColor().y, 0.2f);
    EXPECT_FLOAT_EQ(camera->GetClearColor().z, 0.3f);
    EXPECT_FLOAT_EQ(light->GetColor().x, 0.2f);
    EXPECT_FLOAT_EQ(light->GetColor().y, 0.4f);
    EXPECT_FLOAT_EQ(light->GetColor().z, 0.6f);
    EXPECT_FLOAT_EQ(light->GetIntensity(), 3.0f);
    EXPECT_FLOAT_EQ(light->GetRange(), 12.0f);
    EXPECT_FLOAT_EQ(light->GetOuterCutoff(), 35.0f);
    EXPECT_EQ(light->GetLightType(), NLS::Render::Settings::ELightType::SPOT);

    EXPECT_TRUE(backend.DestroyInstance(instance).Succeeded());
    backend.Shutdown();
}

TEST(ScriptRuntimeTests, LuaRotatingCubeScriptCreatesAndRotatesScenePrimitive)
{
    LuaScriptBackend backend;
    ASSERT_TRUE(backend.Initialize({}).Succeeded());

    ScriptAsset asset;
    asset.assetId = NLS::Core::Assets::AssetId::New();
    asset.language = ScriptLanguage::Lua;
    asset.sourcePath = "Assets/RotatingCube.lua";
    asset.sourceText =
        "local M = {} "
        "function M:Awake() self.cube = self.gameObject:createPrimitive('Cube') end "
        "function M:Update() "
        "local half = Nullus.Time.deltaTime * 0.5; "
        "self.cube.transform.localRotation = {x = 0, y = math.sin(half), z = 0, w = math.cos(half)} "
        "end "
        "return M";
    ASSERT_TRUE(backend.LoadScript(asset).Succeeded());

    NLS::Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("LuaRotatingCubeRoot");
    ScriptInstanceHandle instance;
    ASSERT_TRUE(backend.CreateInstance(
        asset,
        NativeObjectHandle::FromInstanceId(parent.GetInstanceID()),
        instance)
                    .Succeeded());
    ASSERT_TRUE(backend.Invoke(instance, ScriptCallback::Awake, {}).Succeeded());

    auto* cube = scene.FindGameObjectByName("Cube");
    ASSERT_NE(cube, nullptr);
    ScriptInvocationContext update;
    update.frame.deltaTime = 0.5f;
    ASSERT_TRUE(backend.Invoke(instance, ScriptCallback::Update, update).Succeeded());
    ASSERT_NE(cube->GetTransform(), nullptr);
    EXPECT_NE(cube->GetTransform()->GetLocalRotation().y, 0.0f);

    ASSERT_TRUE(backend.DestroyInstance(instance).Succeeded());
    backend.Shutdown();
}

TEST(ScriptRuntimeTests, LuaHotReloadKeepsFieldsAndOnlyRebindsLifecycle)
{
    ScriptApiDatabase api;
    ScriptTypeDescriptor descriptor;
    descriptor.name = "NLS::Scripts::Reloadable";
    descriptor.id = MakeStableScriptId(descriptor.name);
    descriptor.fields.push_back({42, "value", {0, ScriptValueKind::Int32, "int32_t", 4, 4, true}});
    ASSERT_TRUE(api.RegisterType(descriptor));

    auto backend = std::make_unique<LuaScriptBackend>();
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());

    ScriptAsset original;
    original.assetId = NLS::Core::Assets::AssetId::New();
    original.language = ScriptLanguage::Lua;
    original.scriptType = descriptor.id;
    original.sourcePath = "Assets/Reloadable.lua";
    original.sourceText =
        "return { value = 1, Awake = function(self) self.value = self.value + 1 end, "
        "Start = function(self) self.value = self.value + 2 end, "
        "OnDisable = function(self) self.value = self.value + 10 end, "
        "OnEnable = function(self) self.value = self.value + 100 end, "
        "Update = function(self) self.value = self.value + 1000 end, "
        "OnDestroy = function(self) self.value = self.value + 10000 end }";
    ASSERT_TRUE(runtime.LoadScript(original).Succeeded());

    ScriptInstanceHandle instance;
    ASSERT_TRUE(runtime.CreateInstance(original, {}, instance).Succeeded());
    ASSERT_TRUE(runtime.Invoke(instance, ScriptCallback::Awake, {}).Succeeded());
    ASSERT_TRUE(runtime.Invoke(instance, ScriptCallback::Start, {}).Succeeded());

    ScriptValue value;
    ASSERT_TRUE(runtime.GetField(instance, 42, value));
    ASSERT_TRUE(std::holds_alternative<int32_t>(value));
    EXPECT_EQ(std::get<int32_t>(value), 4);

    ScriptAsset replacement = original;
    replacement.sourceText =
        "return { OnDisable = function(self) self.value = self.value + 10 end, "
        "OnEnable = function(self) self.value = self.value + 100 end, "
        "Update = function(self) self.value = self.value + 7 end, "
        "Awake = function(self) self.value = self.value + 10000 end, "
        "OnDestroy = function(self) self.value = self.value + 100000 end }";
    ASSERT_TRUE(runtime.ReloadAtFrameBoundary(replacement, api, ScriptFrameContext{0.016f, 0.016f, 1.0, 1}).Succeeded());

    ASSERT_TRUE(runtime.GetField(instance, 42, value));
    EXPECT_EQ(std::get<int32_t>(value), 114);
    ASSERT_TRUE(runtime.Invoke(instance, ScriptCallback::Update, {}).Succeeded());
    ASSERT_TRUE(runtime.GetField(instance, 42, value));
    EXPECT_EQ(std::get<int32_t>(value), 121);
    ASSERT_TRUE(runtime.DestroyInstance(instance).Succeeded());
}

TEST(ScriptRuntimeTests, LuaHotReloadCompilationFailureLeavesOldInstanceRunning)
{
    auto backend = std::make_unique<LuaScriptBackend>();
    ScriptRuntime runtime;
    ASSERT_TRUE(runtime.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(runtime.Initialize({}).Succeeded());

    ScriptAsset original;
    original.assetId = NLS::Core::Assets::AssetId::New();
    original.language = ScriptLanguage::Lua;
    original.sourcePath = "Assets/Stable.lua";
    original.sourceText = "return { value = 1, Update = function(self) self.value = self.value + 1 end }";
    ASSERT_TRUE(runtime.LoadScript(original).Succeeded());
    ScriptInstanceHandle instance;
    ASSERT_TRUE(runtime.CreateInstance(original, {}, instance).Succeeded());
    ASSERT_TRUE(runtime.Invoke(instance, ScriptCallback::Update, {}).Succeeded());

    ScriptAsset invalid = original;
    invalid.sourceText = "return { Update = function(self)";
    const auto status = runtime.ReloadAtFrameBoundary(invalid, {}, ScriptFrameContext{});
    EXPECT_EQ(status.code, ScriptStatusCode::CompilationFailed);
    EXPECT_TRUE(runtime.Invoke(instance, ScriptCallback::Update, {}).Succeeded());
    ASSERT_TRUE(runtime.DestroyInstance(instance).Succeeded());
}
#endif
