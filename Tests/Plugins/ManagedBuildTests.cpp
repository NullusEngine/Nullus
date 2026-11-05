#include <gtest/gtest.h>
#include "Scripting/ManagedScriptBuildService.h"
#include "Core/Assets/ContentHash.h"
#include "Guid.h"
#include <filesystem>
#include <windows.h>

namespace
{
struct ProjectFixture
{
    std::filesystem::path root=std::filesystem::temp_directory_path() / ("NullusManagedBuild-"+NLS::Guid::New().ToString());
    ProjectFixture() { std::filesystem::create_directories(root / "Assets"); }
    ~ProjectFixture() { std::error_code ignored; std::filesystem::remove_all(root,ignored); }
};
struct LockedAssemblyFile
{
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit LockedAssemblyFile(const std::filesystem::path& path)
        : handle(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)) {}
    ~LockedAssemblyFile() { if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
}
TEST(ManagedPluginBuild, CompiledRevisionChangesAndIdenticalLockedAssemblyCanBeReused)
{
    using namespace NLS::Editor::Scripting;
    ProjectFixture fixture;
    ManagedScriptBuildRequest request; request.projectRoot=fixture.root; request.dotnetPath=NLS_PLUGIN_DOTNET_EXECUTABLE;
    const auto debug=ManagedScriptBuildService::Build(request); ASSERT_TRUE(debug.succeeded) << debug.output;
    const auto original=NLS::Core::Assets::HashContentFile(debug.assembly);
    LockedAssemblyFile old(debug.assembly); ASSERT_NE(old.handle,INVALID_HANDLE_VALUE) << GetLastError();
    request.configuration="Release";
    const auto release=ManagedScriptBuildService::Build(request); ASSERT_TRUE(release.succeeded) << release.output;
    EXPECT_NE(debug.assembly,release.assembly);
    EXPECT_NE(original,NLS::Core::Assets::HashContentFile(release.assembly));
    EXPECT_EQ(original,NLS::Core::Assets::HashContentFile(debug.assembly));
    LockedAssemblyFile current(release.assembly); ASSERT_NE(current.handle,INVALID_HANDLE_VALUE) << GetLastError();
    const auto repeated=ManagedScriptBuildService::Build(request); ASSERT_TRUE(repeated.succeeded) << repeated.output;
    EXPECT_EQ(repeated.assembly,release.assembly);
    EXPECT_EQ(NLS::Core::Assets::HashContentFile(repeated.assembly),NLS::Core::Assets::HashContentFile(release.assembly));
}
