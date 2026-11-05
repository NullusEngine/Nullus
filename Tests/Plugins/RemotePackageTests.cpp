#include <gtest/gtest.h>
#include "Packages/RemotePackageSources.h"
#include "Platform/Process/ProcessRunner.h"
#include "Guid.h"
#include <Json/json.hpp>
#include <fstream>
#include <future>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace NLS::Packages;
namespace
{
std::string ReadText(const std::filesystem::path& p) { std::ifstream file(p); return {(std::istreambuf_iterator<char>(file)),{}}; }
struct RemoteFixture
{
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("NullusRemote-" + NLS::Guid::New().ToString());
    RemotePackageOptions options = DefaultRemotePackageOptions();
    std::string url, commit;
    std::atomic_bool cancel{false};
    std::future<NLS::Platform::Process::ProcessRunResult> server;
    RemoteFixture()
    {
        std::filesystem::create_directories(root);
        NLS::Platform::Process::ProcessRunOptions run;
        run.executable = NLS_TEST_PYTHON; run.arguments = {NLS_REMOTE_FIXTURE,root.generic_string(),options.gitExecutable.generic_string(),NLS_TEST_OPENSSL};
        run.workingDirectory = root; run.cancel = &cancel; run.timeout = std::chrono::minutes(2);
#ifdef _WIN32
        wchar_t windows[32768]{}; GetWindowsDirectoryW(windows,32768);
        run.environment["SystemRoot"] = std::filesystem::path(windows).generic_string();
        run.environment["PATH"] = options.gitExecutable.parent_path().generic_string() + ";" + std::filesystem::path(NLS_TEST_OPENSSL).parent_path().generic_string();
        run.environment["TEMP"] = root.generic_string(); run.environment["TMP"] = root.generic_string();
#else
        run.environment["PATH"] = "/usr/bin:/bin";
#endif
        server = std::async(std::launch::async,[run] { return NLS::Platform::Process::Run(run); });
        for (unsigned i = 0; i < 1000 && !std::filesystem::exists(root / "ready.json"); ++i)
        {
            if (server.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            { const auto result = server.get(); throw std::runtime_error("HTTPS fixture failed: " + result.error + result.output); }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!std::filesystem::exists(root / "ready.json")) { cancel = true; server.get(); throw std::runtime_error("HTTPS fixture startup timed out"); }
        const auto info = nlohmann::json::parse(ReadText(root / "ready.json"));
        url = info.at("url"); commit = info.at("commit"); options.caCertificate = root / "cert.pem";
    }
    ~RemoteFixture() { cancel = true; if (server.valid()) server.get(); std::error_code ignored; std::filesystem::remove_all(root,ignored); }
};
}
TEST(RemotePackages, HttpsDependencyGraphAtomicCommitTlsIntegrityAndRedirects)
{
    RemoteFixture fixture; PackageManager manager(fixture.root / "Project"); ConfigureRemotePackageSources(manager,fixture.options);
    auto result = manager.Install({PackageSourceKind::Registry,fixture.url + "/index.json","org.fixture.root","^1.0.0"});
    ASSERT_TRUE(result.succeeded) << result.error; EXPECT_EQ(manager.List().size(),2u); EXPECT_TRUE(manager.Validate().succeeded);
    const auto before = ReadText(manager.Project() / "Packages/packages-lock.json");
    auto bad = manager.Install({PackageSourceKind::Registry,fixture.url + "/index.json","org.fixture.bad","*"});
    EXPECT_FALSE(bad.succeeded); EXPECT_NE(bad.error.find("integrity"),std::string::npos);
    EXPECT_EQ(before,ReadText(manager.Project() / "Packages/packages-lock.json"));
    RemotePackageOptions untrusted = fixture.options; untrusted.caCertificate.clear();
    RegistryPackageSource tls(untrusted);
    EXPECT_THROW(tls.Candidates({PackageSourceKind::Registry,fixture.url + "/index.json"}),std::exception);
    RegistryPackageSource redirects(fixture.options);
    EXPECT_FALSE(redirects.Candidates({PackageSourceKind::Registry,fixture.url + "/redirect.json"}).empty());
    EXPECT_THROW(redirects.Candidates({PackageSourceKind::Registry,fixture.url + "/loop.json"}),std::exception);
    EXPECT_TRUE(manager.RestoreOffline().succeeded);
}
TEST(RemotePackages, GitFixedCommitObjectExtractionWithoutFiltersAndZipGuards)
{
    RemoteFixture fixture; PackageManager manager(fixture.root / "Project"); ConfigureRemotePackageSources(manager,fixture.options);
    PackageRequest request{PackageSourceKind::Git,fixture.url + "/repo.git"}; request.revision = fixture.commit;
    const auto installed = manager.Install(request); ASSERT_TRUE(installed.succeeded) << installed.error;
    EXPECT_EQ(installed.id,"org.fixture.git"); EXPECT_TRUE(manager.Validate().succeeded);
    request.revision = "main"; EXPECT_FALSE(manager.Install(request).succeeded);
    for (const auto* archive : {"traversal.zip","case.zip","link.zip"})
    {
        const auto result = manager.Install({PackageSourceKind::Zip,(fixture.root / archive).generic_string()});
        EXPECT_FALSE(result.succeeded) << archive;
    }
    PackageLimits tinyLimits; tinyLimits.maxDownloadBytes = 8;
    PackageManager limited(fixture.root / "Limited",tinyLimits); ConfigureRemotePackageSources(limited,fixture.options);
    EXPECT_FALSE(limited.Install({PackageSourceKind::Registry,fixture.url + "/index.json","org.fixture.root","*"}).succeeded);
    EXPECT_FALSE(std::filesystem::exists(fixture.root / "escaped.txt"));
}
