#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Editor::Debug
{
struct VisualStudioInstance
{
    std::filesystem::path installationPath;
    std::string version;
    std::string edition;
    bool compatible = false;
    bool restartRequired = false;
};

// Discovers installed VS 2022 instances without reading PATH. The Windows
// implementation uses the Setup Configuration registry contract; other
// platforms return an empty list because the VSIX integration is Windows-only.
std::vector<VisualStudioInstance> DiscoverVisualStudioInstances();

// Installs the bundled VSIX only after the caller has shown the user the
// instance and received confirmation. The VSIX installer is resolved from the
// selected instance, never from PATH.
bool InstallVisualStudioExtension(
    const VisualStudioInstance& instance,
    const std::filesystem::path& vsixPath,
    bool userConfirmed,
    std::string& errorMessage);
}
