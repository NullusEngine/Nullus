#pragma once

#include "IBuildPlatform.h"

namespace NLS::Build
{
// Windows x64 / DX12 (and Vulkan) desktop adapter.
//
// The adapter validates a prebuilt player template, assembles the install
// tree from the template's runtime manifest plus explicit binary dependency
// checks, applies the product descriptor, optionally signs with Authenticode,
// and produces a ZIP (or a plain directory) for distribution.
class WindowsDesktopPlatform final : public IBuildPlatform
{
public:
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

    // Exposed for tests: decides whether a template file belongs to the
    // installed runtime payload.
    static bool IsRuntimeTemplateFile(const std::filesystem::path& relativePath);
    static std::string ResolveWindowsExecutableName(const BuildProfile& profile);
};
} // namespace NLS::Build
