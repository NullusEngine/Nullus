#pragma once

#include <filesystem>
#include <cstdint>
#include <string>

namespace NLS::Editor::Debug
{
struct VSCodeConfigurationResult
{
    bool success = false;
    std::string errorMessage;
};

// Merges the Nullus script debugger entries into a project's existing VS Code
// files. Existing configurations are retained unless they use one of the
// reserved Nullus names, which are replaced deterministically.
VSCodeConfigurationResult GenerateVSCodeConfiguration(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& visualStudioCodePath = {},
    uint16_t luaPandaPort = 8818,
    bool stopOnEntry = false,
    const std::filesystem::path& projectManifestPath = {});
}
