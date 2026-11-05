#include <gtest/gtest.h>
#include "Scripting/LuaScriptBackend.h"
#include "Scripting/CoreClrScriptBackend.h"
#include "Scripting/ScriptApiManifest.h"
#include "Core/Modules/ModuleSystem.h"
#include <fstream>
#include <thread>
#include "Guid.h"
using namespace NLS::Core::Modules;
namespace
{
void Drain(ModuleManager& modules,const std::string& id)
{
    ASSERT_TRUE(modules.RequestUnload(id));
    for (int i = 0; i < 100 && modules.State(id) != ModuleState::SafeToUnload && modules.State(id) != ModuleState::RestartRequired; ++i)
    { modules.Pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
}
TEST(ScriptPluginAdapter, LuaUsesExistingVmRegistersRunsAndReleasesCoroutine)
{
    NLS::Scripting::LuaScriptBackend backend;
    ASSERT_TRUE(backend.Initialize({}).Succeeded());
    ExtensionRegistry registry; ModuleManager modules(registry,HostKind::Game);
    ModuleDescriptor descriptor{"lua",ModuleType::Runtime,LoadingPhase::PostEngine,3,{},true};
    ASSERT_TRUE(modules.Register(descriptor,[&] { return backend.CreatePluginModule(std::filesystem::path(NLS_PLUGIN_REPO) / "Examples/Plugins/LuaRuntime/Runtime.lua"); }));
    ASSERT_TRUE(modules.Start("lua")) << modules.LastError();
    { auto lease = registry.Find("org.nullus.sample.lua.counter"); ASSERT_TRUE(lease); ExtensionCall call{2}; EXPECT_TRUE(lease.Invoke(call)); EXPECT_EQ(call.value,1u); }
    for (auto& lease : registry.FindKind("runtime.play")) { ExtensionCall call{2}; EXPECT_TRUE(lease.Invoke(call)); }
    Drain(modules,"lua"); EXPECT_EQ(modules.State("lua"),ModuleState::SafeToUnload) << modules.LastError(); EXPECT_EQ(registry.Count(),0u);
    backend.Shutdown();
}
TEST(ScriptPluginAdapter, CoreClrCollectibleUnloadAndStrongHandleDiagnostic)
{
    using namespace NLS::Scripting;
    ScriptApiDatabase database;
    std::ifstream input(std::filesystem::path(NLS_PLUGIN_REPO) / "Library/ScriptApi/ScriptApi.json");
    const std::string json((std::istreambuf_iterator<char>(input)),{}); ASSERT_TRUE(ScriptApiManifest::FromJson(json,database).Succeeded());
    CoreClrScriptBackend backend; backend.SetDotnetRoot(NLS_PLUGIN_DOTNET_ROOT);
    backend.SetHostArtifacts(std::filesystem::path(NLS_PLUGIN_MANAGED_OUTPUT) / "GameScripts.runtimeconfig.json",std::filesystem::path(NLS_PLUGIN_MANAGED_OUTPUT) / "GameScripts.dll");
    ASSERT_TRUE(backend.Initialize(database).Succeeded());
    ExtensionRegistry registry; ModuleManager modules(registry,HostKind::Game);
    const auto assembly = std::filesystem::path(NLS_PLUGIN_SAMPLE_OUTPUT) / "Nullus.SamplePlugin.dll";
    ModuleDescriptor normal{"csharp",ModuleType::Runtime,LoadingPhase::PostEngine,3,{},true};
    ASSERT_TRUE(modules.Register(normal,[&] { return backend.CreatePluginModule(assembly,"Nullus.SamplePlugin.RuntimePlugin"); }));
    ASSERT_TRUE(modules.Start("csharp")) << modules.LastError();
    { auto lease = registry.Find("org.nullus.sample.csharp.counter"); ASSERT_TRUE(lease); ExtensionCall call{2}; EXPECT_TRUE(lease.Invoke(call)); EXPECT_EQ(call.value,1u); }
    Drain(modules,"csharp"); EXPECT_EQ(modules.State("csharp"),ModuleState::SafeToUnload) << modules.LastError();
    normal.id = "pinned"; modules.SetDrainTimeout(std::chrono::milliseconds(30));
    ASSERT_TRUE(modules.Register(normal,[&] { return backend.CreatePluginModule(assembly,"Nullus.SamplePlugin.PinnedPlugin"); }));
    ASSERT_TRUE(modules.Start("pinned")) << modules.LastError();
    Drain(modules,"pinned"); EXPECT_EQ(modules.State("pinned"),ModuleState::RestartRequired);
    EXPECT_NE(modules.LastError().find("AssemblyLoadContext"),std::string::npos); EXPECT_EQ(registry.Count(),0u);
    backend.Shutdown();
}
TEST(ScriptPluginAdapter, LuaStartupRollbackAndCleanupFailureDiagnostic)
{
    const auto path = std::filesystem::temp_directory_path() / ("NullusLuaPlugin-" + NLS::Guid::New().ToString() + ".lua");
    struct File { std::filesystem::path path; ~File() { std::error_code ignored; std::filesystem::remove(path,ignored); } } file{path};
    NLS::Scripting::LuaScriptBackend backend; ASSERT_TRUE(backend.Initialize({}).Succeeded());
    ExtensionRegistry registry; ModuleManager modules(registry,HostKind::Game);
    { std::ofstream output(path); output << "return {startup=function(host) assert(host.register_extension('lua.partial','runtime.play',function() end)); error('partial startup') end}"; }
    ModuleDescriptor descriptor{"partial",ModuleType::Runtime,LoadingPhase::PostEngine,3,{},true};
    ASSERT_TRUE(modules.Register(descriptor,[&] { return backend.CreatePluginModule(path); }));
    EXPECT_FALSE(modules.Start("partial")); EXPECT_EQ(registry.Count(),0u); EXPECT_EQ(modules.State("partial"),ModuleState::Failed);
    { std::ofstream output(path); output << "return {startup=function(host) assert(host.register_extension('lua.cleanup','runtime.play',function() end)) end, shutdown=function() error('owned cleanup failed') end}"; }
    descriptor.id = "cleanup"; ASSERT_TRUE(modules.Register(descriptor,[&] { return backend.CreatePluginModule(path); }));
    ASSERT_TRUE(modules.Start("cleanup")); modules.SetDrainTimeout(std::chrono::milliseconds(0)); Drain(modules,"cleanup");
    EXPECT_EQ(modules.State("cleanup"),ModuleState::RestartRequired); EXPECT_EQ(registry.Count(),0u);
    EXPECT_NE(modules.LastError().find("owned cleanup failed"),std::string::npos);
    backend.Shutdown();
}
