#pragma once

#include "BuildDependencyCollector.h"
#include "BuildPipeline.h"
#include "IBuildPlatform.h"

#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Build
{
namespace Detail
{
// Writes the base and per-pack .npak archives into the staging tree, copies
// raw assets, writes the runtime manifest, Game.ini, and Player.info.
// Returns false with *error set on failure.
bool StageContent(
    const BuildContext& context,
    const DependencyCollectionResult& collection,
    BuildReport& report,
    std::vector<std::string>& contentPackages,
    std::string* error);

// Reopens every staged package, verifies payload hashes, and confirms the
// runtime manifest references only packaged artifacts.
bool VerifyStagedContent(
    const BuildContext& context,
    BuildReport& report,
    const std::vector<std::string>& contentPackages,
    std::string* error);

// Writes Player.info into the staging root.
bool WritePlayerDescriptorFile(
    const BuildContext& context,
    const PlayerTemplateInfo& player,
    const std::vector<std::string>& contentPackages,
    BuildReport& report,
    std::string* error);

// Copies symbol files into a separate symbols directory next to the product.
bool StageSymbols(
    const BuildContext& context,
    const std::vector<std::filesystem::path>& symbolFiles,
    BuildReport& report,
    std::filesystem::path& symbolsDirectory,
    std::string* error);

// Writes the third-party license inventory delivered with the product.
void WriteLicenseInventory(const BuildContext& context, BuildReport& report);
} // namespace Detail
} // namespace NLS::Build
