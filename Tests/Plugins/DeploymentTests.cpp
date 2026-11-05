#include <gtest/gtest.h>
#include "Plugins/RuntimeDeployment.h"
#include "Plugins/PluginManager.h"
#include "Plugins/ApplicationPlugins.h"
#include "Guid.h"
#include <Json/json.hpp>
#include <fstream>
#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace NLS::Plugins;
namespace
{
struct DeploymentFixture
{
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("NullusPlugin-" + NLS::Guid::New().ToString());
    std::filesystem::path project = root / "Project", output = root / "Player";
    DeploymentFixture()
    {
        std::filesystem::create_directories(project / "Plugins/RuntimeSample/Binaries");
        const auto example = std::filesystem::path(NLS_PLUGIN_EXAMPLES) / "RuntimeSample";
        std::filesystem::copy_file(example / "plugin.json",project / "Plugins/RuntimeSample/plugin.json");
        std::filesystem::copy(example / "Content",project / "Plugins/RuntimeSample/Content",std::filesystem::copy_options::recursive);
        std::filesystem::copy_file(NLS_TEST_PLUGIN_PATH,project / "Plugins/RuntimeSample/Binaries" / std::filesystem::path(NLS_TEST_PLUGIN_PATH).filename());
        std::filesystem::create_directories(project / "Packages");
        std::ofstream(project / "Packages/plugins.json") << R"({"schemaVersion":1,"enabled":{"org.nullus.sample.runtime":true}})";
    }
    ~DeploymentFixture() { std::error_code error; std::filesystem::remove_all(root,error); }
};
int RunProbe(const std::filesystem::path& exe, const std::filesystem::path& root)
{
#ifdef _WIN32
    const wchar_t* args[]{exe.c_str(), root.c_str(), nullptr};
    return static_cast<int>(_wspawnv(_P_WAIT,exe.c_str(),args));
#else
    const auto child = fork();
    if (child == 0) { execl(exe.c_str(),exe.c_str(),root.c_str(),nullptr); _exit(127); }
    if (child < 0) return -1;
    int status = 0; if (waitpid(child,&status,0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
}
TEST(RuntimeDeployment, SeparateProcessLoadsActualDllAfterProjectIsRemoved)
{
    DeploymentFixture f; std::string error;
    ASSERT_TRUE(StageRuntimePlugins(f.project,f.output,HostPlatform(),HostArchitecture(),&error)) << error;
    const auto probe = f.output / std::filesystem::path(NLS_TEST_PLUGIN_PROBE).filename();
    std::filesystem::copy_file(NLS_TEST_PLUGIN_PROBE,probe);
#ifdef NLS_TEST_HOST_DLL
    std::filesystem::copy_file(NLS_TEST_HOST_DLL,f.output / std::filesystem::path(NLS_TEST_HOST_DLL).filename());
#endif
    std::filesystem::remove_all(f.project);
    EXPECT_EQ(RunProbe(probe,f.output),0);
}
TEST(RuntimeDeployment, TamperedBinaryIsRejectedBeforeStartup)
{
    DeploymentFixture f; std::string error;
    ASSERT_TRUE(StageRuntimePlugins(f.project,f.output,HostPlatform(),HostArchitecture(),&error)) << error;
    const auto binary = f.output / "Plugins/org.nullus.sample.runtime/Binaries" / std::filesystem::path(NLS_TEST_PLUGIN_PATH).filename();
    std::ofstream(binary,std::ios::binary|std::ios::app) << "tampered";
    NLS::Plugins::PluginHost host(NLS::Core::Modules::HostKind::Game);
    PluginManager manager(host,NLS::Core::Modules::HostKind::Game);
    EXPECT_FALSE(manager.OpenPlayer(f.output)); EXPECT_NE(manager.LastError().find("integrity"),std::string::npos);
    EXPECT_EQ(host.Extensions().Count(),0u);
}
TEST(ProjectPlugins, DifferentNativeVersionRequiresRestart)
{
    DeploymentFixture f;
    PluginHost host(NLS::Core::Modules::HostKind::Editor); PluginManager manager(host,NLS::Core::Modules::HostKind::Editor);
    ASSERT_TRUE(manager.OpenProject(f.project)) << manager.LastError();
    auto other = manager.Active(); ASSERT_EQ(other.size(),1u); other[0].version = SemVersion::Parse("2.0.0");
    EXPECT_EQ(manager.CompareProject(other),ProjectSwitchResult::RestartRequired);
    other = manager.Active(); other[0].source = "different-source";
    EXPECT_EQ(manager.CompareProject(other),ProjectSwitchResult::RestartRequired);
}
TEST(ProjectPlugins, ActualProjectChangeRetainsOldNativeSessionAndRejectsConflict)
{
    DeploymentFixture fixture;
    PluginHost host(NLS::Core::Modules::HostKind::Editor); PluginManager manager(host,NLS::Core::Modules::HostKind::Editor);
    ASSERT_TRUE(manager.OpenProject(fixture.project)) << manager.LastError();
    ASSERT_TRUE(manager.Advance(NLS::Core::Modules::LoadingPhase::PostEngine));
    const auto other=fixture.root / "OtherProject";
    std::filesystem::copy(fixture.project,other,std::filesystem::copy_options::recursive);
    auto json=nlohmann::json::parse(std::ifstream(other / "Plugins/RuntimeSample/plugin.json")); json["version"]="2.0.0";
    std::ofstream(other / "Plugins/RuntimeSample/plugin.json") << json.dump(2);
    const auto generation=manager.Session().Generation();
    EXPECT_FALSE(manager.OpenProject(other)); EXPECT_NE(manager.LastError().find("restart"),std::string::npos);
    EXPECT_EQ(manager.ProjectRoot(),std::filesystem::weakly_canonical(fixture.project)); EXPECT_EQ(manager.Session().Generation(),generation);
    auto lease=host.Extensions().Find("org.nullus.sample.runtime.counter"); ASSERT_TRUE(lease);
    NLS::Core::Modules::ExtensionCall call{2}; EXPECT_TRUE(lease.Invoke(call)); EXPECT_EQ(call.value,1u);
}
TEST(PluginContent, DecomposedFilenameIsReadThroughCanonicalMountAndRetainedAfterUnmount)
{
    DeploymentFixture fixture;
    const auto plugin=fixture.project / "Plugins/RuntimeSample";
    const std::string canonical="Caf\xc3\xa9.txt", physical="Cafe\xcc\x81.txt";
    std::ofstream(plugin / "Content" / std::filesystem::u8path(physical)) << "normalized payload";
    std::ifstream input(plugin / "plugin.json"); auto manifest=nlohmann::json::parse(input); input.close();
    manifest["content"][0]["runtimeKeep"].push_back(canonical);
    std::ofstream(plugin / "plugin.json") << manifest.dump(2);
    auto& table=NLS::Core::Assets::ContentSourceTable::Get();
    std::optional<NLS::Core::Assets::ContentSourceTable::Resolution> held;
    std::filesystem::path logical;
    {
        ApplicationPlugins application(NLS::Core::Modules::HostKind::Editor,fixture.project);
        ASSERT_EQ(application.Manager().ContentRoots().roots.size(),1u);
        logical=application.Manager().ContentRoots().roots[0].logicalPath / std::filesystem::u8path(canonical);
        std::vector<uint8_t> bytes; ASSERT_TRUE(table.ReadAll(logical,bytes));
        EXPECT_EQ(std::string(bytes.begin(),bytes.end()),"normalized payload");
        held=table.Resolve(logical); ASSERT_TRUE(held);
    }
    EXPECT_FALSE(table.Resolve(logical));
    std::vector<uint8_t> bytes; ASSERT_TRUE(held->source->ReadAll(held->relativePath,bytes));
    EXPECT_EQ(std::string(bytes.begin(),bytes.end()),"normalized payload"); held.reset();
    std::string error; ASSERT_TRUE(StageRuntimePlugins(fixture.project,fixture.output,HostPlatform(),HostArchitecture(),&error)) << error;
    EXPECT_TRUE(std::filesystem::is_regular_file(fixture.output / "Plugins/org.nullus.sample.runtime/Content" / std::filesystem::u8path(canonical)));
}
#if defined(_WIN32) && defined(_DEBUG)
TEST(RuntimeDeployment, ShippingRejectsActualDebugRuntimeImports)
{
    DeploymentFixture fixture; std::string error;
    EXPECT_FALSE(StageRuntimePlugins(fixture.project,fixture.output,HostPlatform(),HostArchitecture(),&error,"Release"));
    EXPECT_NE(error.find("debug C/C++ runtime"),std::string::npos) << error;
    EXPECT_FALSE(std::filesystem::exists(fixture.output / "Plugins/runtime-plugins.json"));
}
#endif
