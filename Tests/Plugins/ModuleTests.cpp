#include <gtest/gtest.h>
#include "Plugins/PluginHost.h"
#include "Plugins/SDK/NullusPlugin.h"
#include <stdexcept>

using namespace NLS::Core::Modules;
namespace
{
class TestModule : public IModule
{
public:
    std::function<bool(ModuleContext&)> start;
    std::function<void()> stop;
    bool Startup(ModuleContext& context) override { return start(context); }
    void Shutdown() noexcept override { if (stop) stop(); }
    bool IsStopped() const noexcept override { return true; }
};
auto Factory(std::function<bool(ModuleContext&)> start, std::function<void()> stop = {})
{
    return [start = std::move(start), stop = std::move(stop)] {
        auto result = std::make_unique<TestModule>(); result->start = start; result->stop = stop; return result;
    };
}
ModuleDescriptor Descriptor(const std::string& id, std::vector<std::string> dependencies = {})
{
    ModuleDescriptor d; d.id = id; d.dependencies = std::move(dependencies); d.supportsUnload = true; return d;
}
}
TEST(ModuleTransaction, PartialStartupRollsBackRegistrationsAndResource)
{
    ExtensionRegistry registry; ModuleManager manager(registry, HostKind::Game);
    int released = 0, cancelled = 0;
    RegistrationToken token;
    ASSERT_TRUE(manager.Register(Descriptor("broken"), Factory([&](ModuleContext& c) {
        token = c.extensions.Register(c.scope, {"partial", "test"}, [](ExtensionCall&) {});
        EXPECT_FALSE(c.extensions.Find("partial"));
        c.scope->TrackCleanup([&] { ++released; });
        c.scope->TrackAsync([&] { ++cancelled; }, [] { return true; });
        return false;
    })));
    EXPECT_FALSE(manager.Start("broken"));
    EXPECT_EQ(registry.Count(), 0u); EXPECT_EQ(released, 1); EXPECT_EQ(cancelled, 1);
    EXPECT_EQ(manager.State("broken"), ModuleState::Failed);
}
TEST(ModuleTransaction, ExceptionRollsBackNewDependencies)
{
    ExtensionRegistry registry; ModuleManager manager(registry, HostKind::Game);
    int stopped = 0;
    ASSERT_TRUE(manager.Register(Descriptor("dependency"), Factory([](ModuleContext&) { return true; }, [&] { ++stopped; })));
    ASSERT_TRUE(manager.Register(Descriptor("broken", {"dependency"}), Factory([](ModuleContext&) -> bool { throw std::runtime_error("injected"); })));
    EXPECT_FALSE(manager.Start("broken")); EXPECT_EQ(stopped, 1);
    EXPECT_EQ(manager.State("dependency"), ModuleState::SafeToUnload);
    EXPECT_NE(manager.LastError().find("injected"), std::string::npos);
}
TEST(ModuleTransaction, ExistingSharedDependencyRemainsRunning)
{
    ExtensionRegistry registry; ModuleManager manager(registry, HostKind::Game);
    ASSERT_TRUE(manager.Register(Descriptor("shared"), Factory([](ModuleContext&) { return true; })));
    ASSERT_TRUE(manager.Register(Descriptor("active", {"shared"}), Factory([](ModuleContext&) { return true; })));
    ASSERT_TRUE(manager.Register(Descriptor("broken", {"shared"}), Factory([](ModuleContext&) { return false; })));
    ASSERT_TRUE(manager.Start("active")); EXPECT_FALSE(manager.Start("broken"));
    EXPECT_EQ(manager.State("shared"), ModuleState::Running);
    EXPECT_EQ(manager.State("active"), ModuleState::Running);
}
TEST(ModuleTransaction, DelayedFailedWorkRetainsDependencyUntilDrainThenRollsItBack)
{
    ExtensionRegistry registry; ModuleManager manager(registry,HostKind::Game); bool finished = false;
    manager.Register(Descriptor("dependency"),Factory([](ModuleContext&) { return true; }));
    manager.Register(Descriptor("failure",{"dependency"}),Factory([&](ModuleContext& context) {
        context.scope->TrackAsync([] {},[&] { return finished; }); return false;
    }));
    EXPECT_FALSE(manager.Start("failure")); EXPECT_EQ(manager.State("dependency"),ModuleState::Running);
    finished = true; manager.Pump(); manager.Pump();
    EXPECT_EQ(manager.State("failure"),ModuleState::Failed); EXPECT_EQ(manager.State("dependency"),ModuleState::SafeToUnload);
}
TEST(ModuleDependencies, RejectsCyclePhaseAndTransitiveEditorDependency)
{
    ExtensionRegistry r;
    { ModuleManager m(r, HostKind::Editor);
      m.Register(Descriptor("a", {"b"}), Factory([](ModuleContext&) { return true; }));
      m.Register(Descriptor("b", {"a"}), Factory([](ModuleContext&) { return true; }));
      EXPECT_FALSE(m.Validate()); EXPECT_NE(m.LastError().find("a -> b -> a"), std::string::npos); }
    { ModuleManager m(r, HostKind::Editor);
      auto early = Descriptor("early", {"late"}); early.phase = LoadingPhase::PreEngine;
      m.Register(early, Factory([](ModuleContext&) { return true; }));
      m.Register(Descriptor("late"), Factory([](ModuleContext&) { return true; })); EXPECT_FALSE(m.Validate()); }
    { ModuleManager m(r, HostKind::Editor);
      auto middle = Descriptor("middle", {"editor"}); middle.type = ModuleType::ThirdParty;
      auto editor = Descriptor("editor"); editor.type = ModuleType::Editor;
      m.Register(Descriptor("runtime", {"middle"}), Factory([](ModuleContext&) { return true; }));
      m.Register(middle, Factory([](ModuleContext&) { return true; }));
      m.Register(editor, Factory([](ModuleContext&) { return true; })); EXPECT_FALSE(m.Validate()); }
}
TEST(ModuleUnload, OutstandingLeaseAndTaskPreventCleanup)
{
    ExtensionRegistry r; ModuleManager m(r, HostKind::Game);
    RegistrationToken token; bool finished = false; int cleaned = 0;
    m.Register(Descriptor("owned"), Factory([&](ModuleContext& c) {
        token = r.Register(c.scope, {"owned.extension", "test"}, [](ExtensionCall&) {});
        c.scope->TrackAsync([] {}, [&] { return finished; });
        c.scope->TrackCleanup([&] { ++cleaned; }); return true;
    }));
    ASSERT_TRUE(m.Start("owned")); auto lease = r.Find("owned.extension"); ASSERT_TRUE(lease);
    ASSERT_TRUE(m.RequestUnload("owned")); m.Pump();
    ExtensionCall call; EXPECT_FALSE(lease.Invoke(call)); EXPECT_FALSE(r.Find("owned.extension"));
    EXPECT_EQ(m.State("owned"), ModuleState::Draining); EXPECT_EQ(cleaned, 0);
    lease = {}; m.Pump(); EXPECT_EQ(cleaned, 0);
    finished = true; m.Pump(); EXPECT_EQ(cleaned, 1); EXPECT_EQ(m.State("owned"), ModuleState::SafeToUnload);
}
TEST(NativePlugin, RealDllUsesHostAuthorityAndRunsRuntimeCallback)
{
    NLS::Plugins::PluginHost host(HostKind::Game);
    ASSERT_TRUE(host.RegisterBinary(Descriptor("runtime"), std::filesystem::path(NLS_TEST_PLUGIN_PATH))) << host.LastError();
    ASSERT_TRUE(host.Modules().Start("runtime")) << host.Modules().LastError();
    auto lease = host.Extensions().Find("org.nullus.sample.runtime.counter"); ASSERT_TRUE(lease);
    ExtensionCall call{98}; ASSERT_TRUE(lease.Invoke(call)); EXPECT_EQ(call.value, host.AuthorityId());
    call = {NLS_PLUGIN_EVENT_TICK}; ASSERT_TRUE(lease.Invoke(call)); EXPECT_EQ(call.value, 1u);
    ASSERT_TRUE(lease.Invoke(call)); EXPECT_EQ(call.value, 2u);
    lease = {}; ASSERT_TRUE(host.Modules().RequestUnload("runtime")); host.Modules().Pump();
    EXPECT_EQ(host.Extensions().Count(), 0u); EXPECT_EQ(host.Modules().State("runtime"), ModuleState::SafeToUnload);
}
TEST(NativePlugin, RequestFromInsideDllCallbackIsDeferred)
{
    NLS::Plugins::PluginHost host(HostKind::Game);
    ASSERT_TRUE(host.RegisterBinary(Descriptor("runtime"), std::filesystem::path(NLS_TEST_PLUGIN_PATH)));
    ASSERT_TRUE(host.Modules().Start("runtime"));
    auto lease = host.Extensions().Find("org.nullus.sample.runtime.counter");
    ExtensionCall call{99}; ASSERT_TRUE(lease.Invoke(call));
    EXPECT_EQ(host.Modules().State("runtime"), ModuleState::Running);
    host.Modules().Pump(); EXPECT_EQ(host.Modules().State("runtime"), ModuleState::Draining);
    lease = {}; host.Modules().Pump(); EXPECT_EQ(host.Modules().State("runtime"), ModuleState::SafeToUnload);
}
TEST(NativePlugin, MissingLibraryIsRejected)
{
    NLS::Plugins::PluginHost host(HostKind::Game);
    EXPECT_FALSE(host.RegisterBinary(Descriptor("missing"), std::filesystem::absolute("does-not-exist.dll")));
    EXPECT_EQ(host.Extensions().Count(), 0u);
}
TEST(NativePlugin, RealDllRejectsAbiTargetLifecycleAndExportMismatch)
{
    NLS::Plugins::PluginHost host(HostKind::Game);
    for (const auto& fixture : {std::pair{NLS_TEST_ABI_FIXTURE_1,"ABI"},std::pair{NLS_TEST_ABI_FIXTURE_2,"target"},
        std::pair{NLS_TEST_ABI_FIXTURE_3,"lifecycle"},std::pair{NLS_TEST_ABI_FIXTURE_4,"export"}})
    {
        EXPECT_FALSE(host.RegisterBinary(Descriptor("rejected"),std::filesystem::path(fixture.first)));
        EXPECT_NE(host.LastError().find(fixture.second),std::string::npos) << host.LastError();
    }
    EXPECT_EQ(host.Extensions().Count(),0u); EXPECT_TRUE(host.Modules().Ids().empty());
}
TEST(ModuleUnload, UnsupportedRequestAndActiveDependentsRequireRestartAtBoundary)
{
    ExtensionRegistry registry; ModuleManager modules(registry,HostKind::Game);
    auto descriptor = Descriptor("resident"); descriptor.supportsUnload = false;
    ASSERT_TRUE(modules.Register(descriptor,Factory([](ModuleContext&) { return true; }))); ASSERT_TRUE(modules.Start("resident"));
    EXPECT_TRUE(modules.RequestUnload("resident")); EXPECT_EQ(modules.State("resident"),ModuleState::Running);
    modules.Pump(); EXPECT_EQ(modules.State("resident"),ModuleState::RestartRequired);
    ASSERT_TRUE(modules.Register(Descriptor("dependency"),Factory([](ModuleContext&) { return true; })));
    ASSERT_TRUE(modules.Register(Descriptor("user",{"dependency"}),Factory([](ModuleContext&) { return true; })));
    ASSERT_TRUE(modules.Start("user")); modules.SetDrainTimeout(std::chrono::milliseconds(0));
    EXPECT_TRUE(modules.RequestUnload("dependency")); modules.Pump();
    EXPECT_EQ(modules.State("dependency"),ModuleState::RestartRequired); EXPECT_EQ(modules.State("user"),ModuleState::Running);
}
