#include "BuildProfileIO.h"

#include <Json/json.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace NLS::Build
{
namespace
{
nlohmann::json ScriptBackendsToJson(const std::vector<ScriptBackend>& backends)
{
    nlohmann::json json = nlohmann::json::array();
    for (const auto backend : backends)
        json.push_back(ToString(backend));
    return json;
}

std::vector<ScriptBackend> ScriptBackendsFromJson(const nlohmann::json& json, const BuildTarget& target)
{
    std::vector<ScriptBackend> backends;
    if (json.is_array())
    {
        for (const auto& item : json)
        {
            if (item.is_string())
                backends.push_back(ScriptBackendFromString(item.get<std::string>(), ScriptBackend::Lua));
        }
    }
    if (backends.empty())
    {
        // Desktop keeps both scripting hosts available by default; targets that
        // cannot host one of them reject the profile during validation.
        backends = {ScriptBackend::CSharp, ScriptBackend::Lua};
    }
    std::sort(backends.begin(), backends.end(), [](const ScriptBackend lhs, const ScriptBackend rhs)
    {
        return static_cast<int>(lhs) < static_cast<int>(rhs);
    });
    backends.erase(std::unique(backends.begin(), backends.end()), backends.end());
    return backends;
}

nlohmann::json TargetToJson(const BuildTarget& target)
{
    nlohmann::json json;
    json["platform"] = ToString(target.platform);
    json["architecture"] = ToString(target.architecture);
    json["graphicsBackend"] = ToString(target.graphicsBackend);
    json["minimumOSVersion"] = target.minimumOSVersion;
    json["artifactPlatform"] = target.artifactPlatform;
    json["shaderTarget"] = target.shaderTarget;
    json["textureFormatPolicy"] = target.textureFormatPolicy;
    json["maxTextureSize"] = target.maxTextureSize;
    json["metalLibraryForApple"] = target.metalLibraryForApple;
    json["scriptBackends"] = ScriptBackendsToJson(target.scriptBackends);
    return json;
}

BuildTarget TargetFromJson(const nlohmann::json& json)
{
    BuildTarget target;
    if (json.contains("platform"))
        target.platform = PlatformFamilyFromString(json.value("platform", std::string("windows")), target.platform);
    if (json.contains("architecture"))
        target.architecture = ArchitectureFromString(json.value("architecture", std::string("x64")), target.architecture);
    if (json.contains("graphicsBackend"))
    {
        target.graphicsBackend = GraphicsBackendKindFromString(
            json.value("graphicsBackend", std::string("dx12")),
            target.graphicsBackend);
    }
    target.minimumOSVersion = json.value("minimumOSVersion", target.minimumOSVersion);
    target.artifactPlatform = json.value("artifactPlatform", target.artifactPlatform);
    target.shaderTarget = json.value("shaderTarget", target.shaderTarget);
    target.textureFormatPolicy = json.value("textureFormatPolicy", target.textureFormatPolicy);
    target.maxTextureSize = json.value("maxTextureSize", target.maxTextureSize);
    target.metalLibraryForApple = json.value("metalLibraryForApple", target.metalLibraryForApple);
    if (json.contains("scriptBackends"))
        target.scriptBackends = ScriptBackendsFromJson(json["scriptBackends"], target);
    return target;
}

void AddError(std::vector<BuildDiagnostic>& diagnostics, std::string code, std::string message, std::string detail)
{
    BuildDiagnostic diagnostic;
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.code = std::move(code);
    diagnostic.message = std::move(message);
    diagnostic.detail = std::move(detail);
    diagnostics.push_back(std::move(diagnostic));
}

void AddWarning(std::vector<BuildDiagnostic>& diagnostics, std::string code, std::string message, std::string detail)
{
    BuildDiagnostic diagnostic;
    diagnostic.severity = DiagnosticSeverity::Warning;
    diagnostic.code = std::move(code);
    diagnostic.message = std::move(message);
    diagnostic.detail = std::move(detail);
    diagnostics.push_back(std::move(diagnostic));
}
} // namespace

const char* BuildProfilesFileName()
{
    return "BuildProfiles.json";
}

std::string SerializeBuildProfile(const BuildProfile& profile)
{
    nlohmann::json json;
    json["id"] = profile.id;
    json["displayName"] = profile.displayName;
    json["productName"] = profile.productName;
    json["companyName"] = profile.companyName;
    json["version"] = profile.version;
    json["buildNumber"] = profile.buildNumber;
    json["mode"] = ToString(profile.mode);
    json["target"] = TargetToJson(profile.target);
    json["startScene"] = profile.startScene;
    json["assetPackSelections"] = profile.assetPackSelections;
    json["keepAssetPaths"] = profile.keepAssetPaths;
    json["rawAssetExtensions"] = profile.rawAssetExtensions;
    json["iconPath"] = profile.iconPath;
    json["executableName"] = profile.executableName;
    json["outputFormat"] = ToString(profile.outputFormat);
    json["outputDirectory"] = profile.outputDirectory;
    json["playerTemplatePath"] = profile.playerTemplatePath;
    json["signingProfile"] = profile.signingProfile;
    json["includeDebugSymbols"] = profile.includeDebugSymbols;
    json["customNativeModules"] = profile.customNativeModules;
    json["validateLaunchOnBuild"] = profile.validateLaunchOnBuild;
    return json.dump(2);
}

std::optional<BuildProfile> ParseBuildProfile(const std::string& json, std::vector<BuildDiagnostic>& diagnostics)
{
    try
    {
        const auto parsed = nlohmann::json::parse(json);
        BuildProfile profile;
        profile.id = parsed.value("id", std::string {});
        profile.displayName = parsed.value("displayName", std::string {});
        profile.productName = parsed.value("productName", std::string {});
        profile.companyName = parsed.value("companyName", std::string("Nullus"));
        profile.version = parsed.value("version", std::string("0.1.0"));
        profile.buildNumber = parsed.value("buildNumber", std::string {});
        profile.mode = BuildModeFromString(parsed.value("mode", std::string("Development")), profile.mode);
        if (parsed.contains("target") && parsed["target"].is_object())
            profile.target = TargetFromJson(parsed["target"]);
        if (profile.target.artifactPlatform.empty())
            profile.target.artifactPlatform = DefaultArtifactPlatform(profile.target.platform);
        profile.startScene = parsed.value("startScene", std::string {});
        if (parsed.contains("assetPackSelections"))
            profile.assetPackSelections = parsed["assetPackSelections"].get<std::vector<std::string>>();
        if (parsed.contains("keepAssetPaths"))
            profile.keepAssetPaths = parsed["keepAssetPaths"].get<std::vector<std::string>>();
        if (parsed.contains("rawAssetExtensions"))
            profile.rawAssetExtensions = parsed["rawAssetExtensions"].get<std::vector<std::string>>();
        profile.iconPath = parsed.value("iconPath", std::string {});
        profile.executableName = parsed.value("executableName", std::string {});
        profile.outputFormat = OutputFormatFromString(parsed.value("outputFormat", std::string("zip")), profile.outputFormat);
        profile.outputDirectory = parsed.value("outputDirectory", std::string {});
        profile.playerTemplatePath = parsed.value("playerTemplatePath", std::string {});
        profile.signingProfile = parsed.value("signingProfile", std::string {});
        profile.includeDebugSymbols = parsed.value("includeDebugSymbols", profile.includeDebugSymbols);
        if (parsed.contains("customNativeModules"))
            profile.customNativeModules = parsed["customNativeModules"].get<std::vector<std::string>>();
        profile.validateLaunchOnBuild = parsed.value("validateLaunchOnBuild", profile.validateLaunchOnBuild);
        return profile;
    }
    catch (const std::exception& exception)
    {
        AddError(diagnostics, "build-profile-parse", "Build profile JSON is invalid.", exception.what());
        return std::nullopt;
    }
}

bool LoadBuildProfiles(
    const std::filesystem::path& documentPath,
    BuildProfilesDocument& document,
    std::vector<BuildDiagnostic>& diagnostics)
{
    std::ifstream input(documentPath, std::ios::binary);
    if (!input)
    {
        AddError(
            diagnostics,
            "build-profiles-missing",
            "BuildProfiles.json was not found.",
            documentPath.string());
        return false;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();

    try
    {
        const auto parsed = nlohmann::json::parse(buffer.str());
        document.schemaVersion = parsed.value("schemaVersion", 1u);
        document.defaultProfileId = parsed.value("defaultProfile", std::string {});
        document.profiles.clear();
        if (parsed.contains("profiles") && parsed["profiles"].is_array())
        {
            for (const auto& profileJson : parsed["profiles"])
            {
                auto profile = ParseBuildProfile(profileJson.dump(), diagnostics);
                if (profile.has_value())
                    document.profiles.push_back(std::move(*profile));
            }
        }
    }
    catch (const std::exception& exception)
    {
        AddError(
            diagnostics,
            "build-profiles-parse",
            "BuildProfiles.json is invalid JSON.",
            exception.what());
        return false;
    }

    if (document.profiles.empty())
    {
        AddError(
            diagnostics,
            "build-profiles-empty",
            "BuildProfiles.json contains no profiles.",
            documentPath.string());
        return false;
    }

    std::set<std::string> ids;
    for (const auto& profile : document.profiles)
    {
        if (profile.id.empty())
            AddError(diagnostics, "build-profile-id", "A build profile has no id.", profile.displayName);
        else if (!ids.insert(profile.id).second)
            AddError(diagnostics, "build-profile-duplicate", "Duplicate build profile id.", profile.id);
    }

    if (document.defaultProfileId.empty() ||
        FindBuildProfile(document, document.defaultProfileId) == nullptr)
    {
        document.defaultProfileId = document.profiles.front().id;
    }
    return true;
}

bool SaveBuildProfiles(
    const std::filesystem::path& documentPath,
    const BuildProfilesDocument& document,
    std::vector<BuildDiagnostic>& diagnostics)
{
    nlohmann::json json;
    json["schemaVersion"] = document.schemaVersion;
    json["defaultProfile"] = document.defaultProfileId;
    nlohmann::json profiles = nlohmann::json::array();
    for (const auto& profile : document.profiles)
        profiles.push_back(nlohmann::json::parse(SerializeBuildProfile(profile)));
    json["profiles"] = std::move(profiles);

    std::error_code directoryError;
    if (!documentPath.parent_path().empty())
        std::filesystem::create_directories(documentPath.parent_path(), directoryError);
    std::ofstream output(documentPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        AddError(
            diagnostics,
            "build-profiles-write",
            "Cannot write BuildProfiles.json.",
            documentPath.string());
        return false;
    }
    output << json.dump(2);
    return true;
}

bool ValidateBuildProfile(const BuildProfile& profile, std::vector<BuildDiagnostic>& diagnostics)
{
    const auto before = diagnostics.size();
    if (profile.id.empty())
        AddError(diagnostics, "build-profile-id", "Build profile needs an id.", profile.displayName);
    if (profile.productName.empty())
        AddWarning(diagnostics, "build-profile-product", "Build profile has no product name; the project name is used.", profile.id);
    if (profile.executableName.empty())
        AddError(diagnostics, "build-profile-executable", "Build profile needs an executable name.", profile.id);
    if (profile.startScene.empty())
        AddError(diagnostics, "build-profile-scene", "Build profile needs a start scene.", profile.id);
    if (profile.version.empty())
        AddWarning(diagnostics, "build-profile-version", "Build profile has no version.", profile.id);

    if (profile.target.platform == PlatformFamily::Unknown ||
        profile.target.architecture == Architecture::Unknown)
    {
        AddError(
            diagnostics,
            "build-profile-target",
            "Build profile target is incomplete (platform/architecture).",
            BuildTargetId(profile.target));
    }
    if (profile.target.graphicsBackend == GraphicsBackendKind::None)
        AddError(diagnostics, "build-profile-backend", "Build profile needs a graphics backend.", profile.id);

    const bool isApple = profile.target.platform == PlatformFamily::MacOS ||
        profile.target.platform == PlatformFamily::IOS;
    if (isApple && profile.target.graphicsBackend != GraphicsBackendKind::Metal)
        AddError(diagnostics, "build-profile-apple-backend", "Apple targets require the Metal backend.", profile.id);
    if (profile.target.platform == PlatformFamily::Windows &&
        profile.target.graphicsBackend != GraphicsBackendKind::DX12 &&
        profile.target.graphicsBackend != GraphicsBackendKind::Vulkan)
    {
        AddWarning(
            diagnostics,
            "build-profile-windows-backend",
            "Windows desktop targets usually ship DX12; the selected backend still has to pass device validation.",
            profile.id);
    }

    if (profile.mode == BuildMode::Shipping && !profile.customNativeModules.empty())
    {
        AddError(
            diagnostics,
            "build-profile-native-modules",
            "Shipping builds with custom native modules require a source-built player template.",
            profile.id);
    }
    if (profile.target.scriptBackends.empty())
        AddError(diagnostics, "build-profile-scripts", "Build profile needs at least one script backend.", profile.id);

    if (profile.mode == BuildMode::Shipping && profile.signingProfile.empty())
    {
        AddWarning(
            diagnostics,
            "build-profile-signing",
            "Shipping profile has no signing credential reference; the output stays unsigned.",
            profile.id);
    }
    return static_cast<size_t>(std::count_if(
        diagnostics.begin() + static_cast<std::ptrdiff_t>(before),
        diagnostics.end(),
        [](const BuildDiagnostic& diagnostic) { return diagnostic.IsError(); })) == 0u;
}

const BuildProfile* FindBuildProfile(const BuildProfilesDocument& document, const std::string& profileId)
{
    if (profileId.empty())
        return document.profiles.empty() ? nullptr : &document.profiles.front();
    for (const auto& profile : document.profiles)
    {
        if (profile.id == profileId)
            return &profile;
    }
    return nullptr;
}

BuildProfile MakeDefaultBuildProfile(
    const std::string& executableName,
    const std::string& productName,
    const std::string& startScene,
    const PlatformFamily hostPlatform,
    const Architecture hostArchitecture,
    const GraphicsBackendKind hostBackend)
{
    BuildProfile profile;
    profile.id = std::string(ToString(hostPlatform)) + "-" +
        (hostBackend == GraphicsBackendKind::DX12 ? "shipping" : "shipping");
    profile.displayName = std::string("Desktop ") + ToString(hostBackend);
    profile.productName = productName;
    profile.executableName = executableName;
    profile.startScene = startScene;
    profile.mode = BuildMode::Shipping;
    profile.target.platform = hostPlatform;
    profile.target.architecture = hostArchitecture;
    profile.target.graphicsBackend = hostBackend;
    profile.target.artifactPlatform = DefaultArtifactPlatform(hostPlatform);
    switch (hostPlatform)
    {
    case PlatformFamily::Windows: profile.target.shaderTarget = "dxil"; break;
    case PlatformFamily::Linux:
    case PlatformFamily::Android: profile.target.shaderTarget = "spirv"; break;
    case PlatformFamily::MacOS:
    case PlatformFamily::IOS: profile.target.shaderTarget = "metal"; break;
    case PlatformFamily::Web: profile.target.shaderTarget = "wgsl"; break;
    case PlatformFamily::Unknown: break;
    }
    profile.target.minimumOSVersion = hostPlatform == PlatformFamily::Windows ? "10.0.19045"
        : hostPlatform == PlatformFamily::Linux ? "glibc-2.35"
        : hostPlatform == PlatformFamily::MacOS ? "13.0"
        : std::string {};
    return profile;
}
} // namespace NLS::Build
