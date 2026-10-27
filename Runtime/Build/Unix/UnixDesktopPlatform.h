#pragma once

#include "IBuildPlatform.h"

namespace NLS::Build
{
// Linux x64 (Vulkan/OpenGL) and macOS (Metal) desktop adapter.
//
// Both platforms share the same install layout and ZIP/directory output. A
// template is a directory containing the Game executable, its shared
// libraries, Assets/, and optional Scripts//Runtime/dotnet payloads. macOS also
// accepts a Game.app bundle.
class NLS_BUILD_API UnixDesktopPlatform final : public IBuildPlatform
{
public:
    explicit UnixDesktopPlatform(PlatformFamily family);

    std::string GetAdapterId() const override;
    PlatformCapability GetCapability(const BuildTarget& target) const override;
    bool CheckToolchain(const BuildContext& context, BuildReport& report, std::string* error) override;
    std::optional<PlayerTemplateInfo> ResolvePlayerTemplate(
        const BuildContext& context,
        BuildReport& report,
        std::string* error) override;
    bool AssemblePlayer(
        const BuildContext& context,
        const PlayerTemplateInfo& player,
        BuildReport& report,
        std::string* error) override;
    bool FinalizeStagingPlayer(const BuildContext& context, BuildReport& report, std::string* error) override;
    bool SignStaging(const BuildContext& context, BuildReport& report, std::string* error) override;
    bool PackageOutput(
        const BuildContext& context,
        const std::filesystem::path& stagingDirectory,
        BuildReport& report,
        std::filesystem::path& outputRoot,
        std::string* error) override;
    bool ValidateOutput(
        const BuildContext& context,
        const std::filesystem::path& packageDirectory,
        BuildReport& report,
        std::string* error) override;

    static bool IsRuntimeTemplateFile(const std::filesystem::path& relativePath);

private:
    PlatformFamily m_family;
};

NLS_BUILD_API void RegisterLinuxDesktopPlatform();
NLS_BUILD_API void RegisterMacOSDesktopPlatform();
} // namespace NLS::Build
