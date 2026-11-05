#include <gtest/gtest.h>
#include "Packages/PackageManager.h"
#include "Plugins/PluginManager.h"
#include "Plugins/ApplicationPlugins.h"
#include "Plugins/RuntimeDeployment.h"
#include "Platform/Process/ProcessRunner.h"
#include "Platform/Process/InterprocessFileLock.h"
#include "Guid.h"
#include <Json/json.hpp>
#include <fstream>
#include <future>
using namespace NLS::Packages;
namespace
{
struct PackageFixture
{
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("NullusPackage-" + NLS::Guid::New().ToString());
    std::filesystem::path project = root / "Project";
    PackageFixture() { std::filesystem::create_directories(project); }
    ~PackageFixture() { std::error_code error; std::filesystem::remove_all(root,error); }
    std::filesystem::path Make(const std::string& id, const std::string& version = "1.0.0")
    {
        auto source = root / (id + "-" + version); std::filesystem::create_directories(source);
        nlohmann::json manifest{{"schemaVersion",1},{"id",id},{"name",id},{"version",version},{"engineVersion","^1.0.0"},{"pluginApiVersion",1},{"modules",nlohmann::json::array()}};
        std::ofstream(source / "plugin.json") << manifest.dump(2);
        std::ofstream(source / "data.txt") << "payload";
        return source;
    }
};
NLS::Platform::Process::ProcessRunResult Probe(std::vector<std::string> args)
{
    NLS::Platform::Process::ProcessRunOptions options;
    options.executable = NLS_PACKAGE_PROBE; options.arguments = std::move(args); options.timeout = std::chrono::seconds(20);
    return NLS::Platform::Process::Run(options);
}
std::string Text(const std::filesystem::path& p) { std::ifstream f(p); return std::string((std::istreambuf_iterator<char>(f)),{}); }
}
TEST(PackageInstall, RealDirectoryInstallLockOfflineRestoreAndUninstall)
{
    PackageFixture f; const auto source = f.Make("org.example.one"); PackageManager manager(f.project);
    const auto installed = manager.Install({PackageSourceKind::Directory,source.generic_string()});
    ASSERT_TRUE(installed.succeeded) << installed.error; EXPECT_EQ(installed.contentHash.size(),64u);
    ASSERT_EQ(manager.List().size(),1u); EXPECT_TRUE(manager.Validate().succeeded);
    auto config = NLS::Plugins::PluginManager::ReadConfiguration(f.project); EXPECT_FALSE(config.enabled.at("org.example.one"));
    EXPECT_TRUE(manager.SetEnabled("org.example.one",true).succeeded);
    EXPECT_TRUE(manager.RestoreOffline().succeeded);
    std::filesystem::remove_all(source); EXPECT_TRUE(manager.RestoreOffline().succeeded);
    EXPECT_TRUE(manager.Uninstall("org.example.one").succeeded); EXPECT_TRUE(manager.List().empty());
}
TEST(PackageInstall, UpdateKeepsGuidIndependentDataAndInvalidHashDoesNotChangeProject)
{
    PackageFixture f; PackageManager manager(f.project); auto source = f.Make("org.example.one");
    auto first = manager.Install({PackageSourceKind::Directory,source.generic_string()}); ASSERT_TRUE(first.succeeded) << first.error;
    const auto before = Text(f.project / "Packages/packages-lock.json");
    PackageRequest bad{PackageSourceKind::Directory,source.generic_string()}; bad.expectedHash = std::string(64,'0');
    EXPECT_FALSE(manager.Install(bad).succeeded); EXPECT_EQ(Text(f.project / "Packages/packages-lock.json"),before);
    source = f.Make("org.example.one","1.1.0");
    auto updated = manager.Install({PackageSourceKind::Directory,source.generic_string()}); ASSERT_TRUE(updated.succeeded) << updated.error;
    EXPECT_EQ(updated.version,"1.1.0"); EXPECT_NE(updated.contentHash,first.contentHash);
}
TEST(PackageConcurrency, IndependentProcessesMergeDifferentInstalls)
{
    PackageFixture f; const auto a = f.Make("org.example.a"), b = f.Make("org.example.b");
    auto first = std::async(std::launch::async,[&] { return Probe({"install",f.project.generic_string(),a.generic_string()}); });
    auto second = std::async(std::launch::async,[&] { return Probe({"install",f.project.generic_string(),b.generic_string()}); });
    auto ar = first.get(), br = second.get(); ASSERT_TRUE(ar.Succeeded()) << ar.error; ASSERT_TRUE(br.Succeeded()) << br.error;
    PackageManager manager(f.project); EXPECT_EQ(manager.List().size(),2u); EXPECT_TRUE(manager.Validate().succeeded);
}
TEST(PackageConcurrency, IndependentProcessesShareOneImmutableCacheEntry)
{
    PackageFixture f; const auto source = f.Make("org.example.shared");
    auto run = [&] { return Probe({"install",f.project.generic_string(),source.generic_string()}); };
    auto first = std::async(std::launch::async,run), second = std::async(std::launch::async,run);
    auto a = first.get(), b = second.get(); ASSERT_TRUE(a.Succeeded()) << a.error; ASSERT_TRUE(b.Succeeded()) << b.error;
    PackageManager manager(f.project); EXPECT_EQ(manager.List().size(),1u); EXPECT_TRUE(manager.Validate().succeeded);
}
TEST(PackageRecovery, ProcessCrashBeforeCommitRollsBackAndCommittedCrashRollsForward)
{
    for (const auto& point : {"prepared","replaced:manifest.json","committed"})
    {
        PackageFixture f; PackageManager manager(f.project); const auto old = f.Make("org.example.old"), added = f.Make("org.example.new");
        ASSERT_TRUE(manager.Install({PackageSourceKind::Directory,old.generic_string()}).succeeded);
        const auto crashed = Probe({"install",f.project.generic_string(),added.generic_string(),point}); ASSERT_EQ(crashed.exitCode,87) << crashed.error;
        manager.Recover(); EXPECT_TRUE(manager.Validate().succeeded);
        EXPECT_EQ(manager.List().size(),std::string(point) == "committed" ? 2u : 1u);
        EXPECT_FALSE(std::filesystem::exists(f.project / "Packages/.transaction.json"));
    }
}
TEST(PackageLock, SeparateProcessCannotEnterAnOwnedProjectLock)
{
    PackageFixture f; const auto path = f.project / "Packages/.write.lock";
    { NLS::Platform::Process::InterprocessFileLock lock(path); EXPECT_EQ(Probe({"lock",path.generic_string()}).exitCode,6); }
    EXPECT_TRUE(Probe({"lock",path.generic_string()}).Succeeded());
}
TEST(PackageInstall, ExistingReaderLeaseDoesNotBlockIdenticalInstall)
{
    PackageFixture f; const auto source = f.Make("org.example.read"); PackageManager manager(f.project);
    auto first = manager.Install({PackageSourceKind::Directory,source.generic_string()}); ASSERT_TRUE(first.succeeded) << first.error;
    NLS::Platform::Process::InterprocessFileLock lease(manager.CacheRoot() / ".locks" / (first.contentHash + ".lock"),false);
    auto again = manager.Install({PackageSourceKind::Directory,source.generic_string()}); EXPECT_TRUE(again.succeeded) << again.error;
}
TEST(PackageRecovery, RepairsCorruptCacheFromLockedSource)
{
    PackageFixture f; const auto source = f.Make("org.example.repair"); PackageManager manager(f.project);
    auto first = manager.Install({PackageSourceKind::Directory,source.generic_string()}); ASSERT_TRUE(first.succeeded) << first.error;
    std::ofstream(manager.CacheRoot() / first.contentHash / "content/data.txt") << "corruption";
    EXPECT_FALSE(manager.Validate().succeeded);
    const auto repaired = manager.Repair(); EXPECT_TRUE(repaired.succeeded) << repaired.error; EXPECT_TRUE(manager.Validate().succeeded);
}
TEST(PackageInstall, UnsupportedProjectSchemaDoesNotModifyConfiguration)
{
    PackageFixture fixture; PackageManager manager(fixture.project); const auto source=fixture.Make("org.example.schema");
    const auto manifest=fixture.project / "Packages/manifest.json";
    const std::string original=R"({"schemaVersion":99,"dependencies":{},"futureData":[1,2,3]})";
    std::ofstream(manifest) << original;
    auto result=manager.Install({PackageSourceKind::Directory,source.generic_string()});
    EXPECT_FALSE(result.succeeded); EXPECT_NE(result.error.find("schema"),std::string::npos);
    EXPECT_EQ(Text(manifest),original); EXPECT_FALSE(std::filesystem::exists(fixture.project / "Packages/packages-lock.json"));
}
TEST(PackageRecovery, RelativeInstallSourceCanBeRepairedFromAnotherWorkingDirectory)
{
    PackageFixture fixture; const auto source=fixture.Make("org.example.relative");
    NLS::Platform::Process::ProcessRunOptions options;
    options.executable=NLS_PACKAGE_PROBE; options.workingDirectory=fixture.root;
    options.arguments={"install",fixture.project.generic_string(),source.filename().generic_string()};
    const auto installed=NLS::Platform::Process::Run(options); ASSERT_TRUE(installed.Succeeded()) << installed.output << installed.error;
    const auto lock=nlohmann::json::parse(Text(fixture.project / "Packages/packages-lock.json"));
    const auto& entry=lock.at("packages").at(0);
    EXPECT_TRUE(std::filesystem::u8path(entry.at("source").get<std::string>()).is_absolute());
    PackageManager manager(fixture.project);
    std::ofstream(manager.CacheRoot() / entry.at("hash").get<std::string>() / "content/data.txt") << "corrupted";
    const auto repaired=manager.Repair(); EXPECT_TRUE(repaired.succeeded) << repaired.error;
    EXPECT_TRUE(manager.Validate().succeeded);
}
TEST(PackageLock, ContentHandleRetainsCacheReadLeaseAfterSessionDestruction)
{
    PackageFixture fixture; const auto source=fixture.Make("org.example.content");
    const auto manifestPath=source / "plugin.json";
    auto manifest=nlohmann::json::parse(Text(manifestPath));
    manifest["content"]=nlohmann::json::array({{{"name","content"},{"path","Content"}}});
    std::filesystem::create_directory(source / "Content"); std::ofstream(source / "Content/read.txt") << "leased data";
    std::ofstream(manifestPath) << manifest.dump(2);
    PackageManager manager(fixture.project); auto installed=manager.Install({PackageSourceKind::Directory,source.generic_string()});
    ASSERT_TRUE(installed.succeeded) << installed.error; ASSERT_TRUE(manager.SetEnabled(installed.id,true).succeeded);
    std::optional<NLS::Core::Assets::ContentSourceTable::Resolution> held;
    {
        NLS::Plugins::ApplicationPlugins application(NLS::Core::Modules::HostKind::Editor,fixture.project);
        held=NLS::Core::Assets::ContentSourceTable::Get().Resolve(fixture.project / "Assets/Plugins/org.example.content/content/read.txt");
        ASSERT_TRUE(held);
    }
    using Lock=NLS::Platform::Process::InterprocessFileLock;
    const auto path=manager.CacheRoot() / ".locks" / (installed.contentHash + ".lock");
    EXPECT_THROW(Lock(path,true,std::chrono::milliseconds(25)),std::exception);
    std::vector<uint8_t> bytes; ASSERT_TRUE(held->source->ReadAll(held->relativePath,bytes));
    EXPECT_EQ(std::string(bytes.begin(),bytes.end()),"leased data"); held.reset();
    EXPECT_NO_THROW(Lock(path,true,std::chrono::milliseconds(100)));
}
TEST(PackageBuildTrust, ApprovesResolvedSourceAndIncludesIndirectBuildScripts)
{
    PackageFixture fixture; const std::string id="org.example.build";
    const auto older=fixture.Make(id,"1.0.0"), selected=fixture.Make(id,"2.0.0");
    for (const auto& root : {older,selected})
    {
        auto manifest=nlohmann::json::parse(Text(root / "plugin.json"));
        manifest["modules"]=nlohmann::json::array({{{"name","Runtime"},{"type","Runtime"},{"loadingPhase","PostEngine"},{"sourceTarget","FixtureSource"}}});
        std::ofstream(root / "plugin.json") << manifest.dump(2);
        std::ofstream(root / "CMakeLists.txt") << "message(FATAL_ERROR \"install and approval must never execute this file\")\n";
        std::ofstream(root / "build.py") << "print('indirect build input')\n";
    }
    PackageManager manager(fixture.project); const auto installed=manager.Install({PackageSourceKind::Directory,selected.generic_string()});
    ASSERT_TRUE(installed.succeeded) << installed.error;
    std::filesystem::create_directories(fixture.project / "Plugins");
    std::filesystem::copy(older,fixture.project / "Plugins/Older",std::filesystem::copy_options::recursive);
    const auto approved=manager.ApproveSourceBuild(id); ASSERT_TRUE(approved.succeeded) << approved.error;
    const auto trust=nlohmann::json::parse(Text(fixture.project / "Packages/build-trust.json")).at("packages").at(id);
    EXPECT_EQ(trust.at("source"),std::filesystem::weakly_canonical(selected).generic_string());
    EXPECT_EQ(trust.at("inputScope"),"package-content-v1");
    const auto cached=manager.CacheRoot() / installed.contentHash / "content";
    EXPECT_EQ(trust.at("buildHash"),NLS::Plugins::HashPluginBuildApprovalInputs(cached));
    std::ofstream(cached / "build.py") << "print('changed without changing CMake')\n";
    EXPECT_NE(trust.at("buildHash"),NLS::Plugins::HashPluginBuildApprovalInputs(cached));
}
