#include "Engine/Assets/PlayerDescriptor.h"

#include <Json/json.hpp>

#include <fstream>
#include <sstream>

namespace NLS::Engine::Assets
{
namespace
{
void AddIssue(
    std::vector<PlayerCompatibilityIssue>& issues,
    std::string code,
    std::string message)
{
    issues.push_back({std::move(code), std::move(message)});
}
} // namespace

const char* PlayerDescriptor::FileName()
{
    return "Player.info";
}

std::string PlayerDescriptor::ToJson() const
{
    nlohmann::json json;
    json["schemaVersion"] = schemaVersion;
    json["playerId"] = playerId;
    json["engineVersion"] = engineVersion;
    json["targetId"] = targetId;
    json["artifactPlatform"] = artifactPlatform;
    json["graphicsBackend"] = graphicsBackend;
    json["buildMode"] = buildMode;
    json["profileId"] = profileId;
    json["productName"] = productName;
    json["companyName"] = companyName;
    json["productVersion"] = productVersion;
    json["buildNumber"] = buildNumber;
    json["startScene"] = startScene;
    json["runtimeManifestPath"] = runtimeManifestPath;
    json["contentRoot"] = contentRoot;
    json["contentPackages"] = contentPackages;
    json["scriptBackends"] = scriptBackends;
    json["scripts"] = {
        {"dotnetRoot", scripts.dotnetRoot},
        {"managedRuntimeConfig", scripts.managedRuntimeConfig},
        {"managedAssembly", scripts.managedAssembly},
        {"scriptApiDirectory", scripts.scriptApiDirectory}};
    json["buildInputHash"] = buildInputHash;
    json["buildReportPath"] = buildReportPath;
    json["signed"] = signedBuild;
    json["signingDescription"] = signingDescription;
    return json.dump(2);
}

std::optional<PlayerDescriptor> PlayerDescriptor::FromJson(const std::string& json, std::string* error)
{
    try
    {
        const auto parsed = nlohmann::json::parse(json);
        PlayerDescriptor descriptor;
        descriptor.schemaVersion = parsed.value("schemaVersion", 1u);
        descriptor.playerId = parsed.value("playerId", std::string {});
        descriptor.engineVersion = parsed.value("engineVersion", std::string {});
        descriptor.targetId = parsed.value("targetId", std::string {});
        descriptor.artifactPlatform = parsed.value("artifactPlatform", std::string {});
        descriptor.graphicsBackend = parsed.value("graphicsBackend", std::string {});
        descriptor.buildMode = parsed.value("buildMode", std::string {});
        descriptor.profileId = parsed.value("profileId", std::string {});
        descriptor.productName = parsed.value("productName", std::string {});
        descriptor.companyName = parsed.value("companyName", std::string {});
        descriptor.productVersion = parsed.value("productVersion", std::string {});
        descriptor.buildNumber = parsed.value("buildNumber", std::string {});
        descriptor.startScene = parsed.value("startScene", std::string {});
        descriptor.runtimeManifestPath = parsed.value("runtimeManifestPath", std::string {});
        descriptor.contentRoot = parsed.value("contentRoot", std::string {});
        if (descriptor.contentRoot.empty())
            descriptor.contentRoot = "Data/User";
        if (parsed.contains("contentPackages"))
            descriptor.contentPackages = parsed["contentPackages"].get<std::vector<std::string>>();
        if (parsed.contains("scriptBackends"))
            descriptor.scriptBackends = parsed["scriptBackends"].get<std::vector<std::string>>();
        if (parsed.contains("scripts") && parsed["scripts"].is_object())
        {
            const auto& scripts = parsed["scripts"];
            descriptor.scripts.dotnetRoot = scripts.value("dotnetRoot", std::string {});
            descriptor.scripts.managedRuntimeConfig = scripts.value("managedRuntimeConfig", std::string {});
            descriptor.scripts.managedAssembly = scripts.value("managedAssembly", std::string {});
            descriptor.scripts.scriptApiDirectory = scripts.value("scriptApiDirectory", std::string {});
        }
        descriptor.buildInputHash = parsed.value("buildInputHash", std::string {});
        descriptor.buildReportPath = parsed.value("buildReportPath", std::string {});
        descriptor.signedBuild = parsed.value("signed", false);
        descriptor.signingDescription = parsed.value("signingDescription", std::string {});
        return descriptor;
    }
    catch (const std::exception& exception)
    {
        if (error != nullptr)
            *error = exception.what();
        return std::nullopt;
    }
}

bool PlayerDescriptor::Save(const std::filesystem::path& path, std::string* error) const
{
    std::error_code directoryError;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), directoryError);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
            *error = "cannot write player descriptor '" + path.string() + "'";
        return false;
    }
    output << ToJson();
    return output.good();
}

std::optional<PlayerDescriptor> PlayerDescriptor::Load(const std::filesystem::path& path, std::string* error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
            *error = "cannot read player descriptor '" + path.string() + "'";
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    return FromJson(buffer.str(), error);
}

std::optional<PlayerDescriptor> PlayerDescriptor::LoadForInstallRoot(
    const std::filesystem::path& installRoot,
    std::string* error)
{
    if (installRoot.empty())
        return std::nullopt;
    return Load(installRoot / FileName(), error);
}

std::vector<PlayerCompatibilityIssue> CheckPlayerCompatibility(
    const PlayerDescriptor& descriptor,
    const std::string& engineVersion,
    const std::string& runtimeTargetId,
    const uint32_t packageFormatVersion)
{
    std::vector<PlayerCompatibilityIssue> issues;
    if (descriptor.schemaVersion == 0u || descriptor.schemaVersion > 1u)
    {
        AddIssue(
            issues,
            "player-descriptor-schema",
            "The player descriptor schema version is not supported by this runtime.");
    }
    if (!descriptor.engineVersion.empty() && !engineVersion.empty() &&
        descriptor.engineVersion != engineVersion)
    {
        AddIssue(
            issues,
            "player-engine-version-mismatch",
            "The installed player was built for engine '" + descriptor.engineVersion +
                "' but this runtime is '" + engineVersion + "'.");
    }
    if (!descriptor.targetId.empty() && !runtimeTargetId.empty() &&
        descriptor.targetId != runtimeTargetId)
    {
        AddIssue(
            issues,
            "player-target-mismatch",
            "The installed player targets '" + descriptor.targetId +
                "' but the runtime resolved '" + runtimeTargetId + "'.");
    }
    if (packageFormatVersion != 0u && descriptor.schemaVersion != packageFormatVersion)
    {
        AddIssue(
            issues,
            "player-package-format-mismatch",
            "The shipped content container version does not match the player runtime.");
    }
    return issues;
}
} // namespace NLS::Engine::Assets
