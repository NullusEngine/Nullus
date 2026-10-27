#pragma once

#include "BuildDef.h"
#include "BuildTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Build
{
// Project-versioned build configuration file. The document keeps every
// profile so an editor panel and NullusBuild read the same source of truth.
struct BuildProfilesDocument
{
    uint32_t schemaVersion = 1u;
    std::string defaultProfileId;
    std::vector<BuildProfile> profiles;
};

NLS_BUILD_API const char* BuildProfilesFileName();

// Loads and validates the document. Validation errors are reported through
// diagnostics; a document with unrelated errors still loads so the editor can
// show and repair it.
NLS_BUILD_API bool LoadBuildProfiles(
    const std::filesystem::path& documentPath,
    BuildProfilesDocument& document,
    std::vector<BuildDiagnostic>& diagnostics);

NLS_BUILD_API bool SaveBuildProfiles(
    const std::filesystem::path& documentPath,
    const BuildProfilesDocument& document,
    std::vector<BuildDiagnostic>& diagnostics);

// Serialises one profile (used for hashing and for the BuildProfile panel).
NLS_BUILD_API std::string SerializeBuildProfile(const BuildProfile& profile);

NLS_BUILD_API std::optional<BuildProfile> ParseBuildProfile(
    const std::string& json,
    std::vector<BuildDiagnostic>& diagnostics);

// A profile is valid when its required fields are present and its target is
// self-consistent. Content availability is checked later by the pipeline
// because it depends on the project.
NLS_BUILD_API bool ValidateBuildProfile(
    const BuildProfile& profile,
    std::vector<BuildDiagnostic>& diagnostics);

NLS_BUILD_API const BuildProfile* FindBuildProfile(
    const BuildProfilesDocument& document,
    const std::string& profileId);

// Builds the implicit default profile from project settings when the project
// has no BuildProfiles.json yet. The default targets the host desktop platform
// so a fresh project can ship without extra configuration.
NLS_BUILD_API BuildProfile MakeDefaultBuildProfile(
    const std::string& executableName,
    const std::string& productName,
    const std::string& startScene,
    PlatformFamily hostPlatform,
    Architecture hostArchitecture,
    GraphicsBackendKind hostBackend);
} // namespace NLS::Build
