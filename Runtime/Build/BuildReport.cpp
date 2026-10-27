#include "BuildReport.h"

#include "Assets/ArtifactManifest.h"

#include <Json/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace NLS::Build
{
namespace
{
const char* SeverityText(const DiagnosticSeverity severity)
{
    switch (severity)
    {
    case DiagnosticSeverity::Info: return "info";
    case DiagnosticSeverity::Warning: return "warning";
    case DiagnosticSeverity::Error: return "error";
    }
    return "info";
}

DiagnosticSeverity SeverityFromText(const std::string& text)
{
    if (text == "error")
        return DiagnosticSeverity::Error;
    if (text == "warning")
        return DiagnosticSeverity::Warning;
    return DiagnosticSeverity::Info;
}
} // namespace

bool BuildReport::HasErrors() const
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [](const BuildDiagnostic& diagnostic) { return diagnostic.IsError(); });
}

size_t BuildReport::ErrorCount() const
{
    return static_cast<size_t>(std::count_if(
        diagnostics.begin(),
        diagnostics.end(),
        [](const BuildDiagnostic& diagnostic) { return diagnostic.IsError(); }));
}

size_t BuildReport::WarningCount() const
{
    return static_cast<size_t>(std::count_if(
        diagnostics.begin(),
        diagnostics.end(),
        [](const BuildDiagnostic& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Warning; }));
}

void BuildReport::AddDiagnostic(
    const DiagnosticSeverity severity,
    std::string code,
    std::string message,
    std::string detail)
{
    BuildDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.code = std::move(code);
    diagnostic.message = std::move(message);
    diagnostic.detail = std::move(detail);
    diagnostics.push_back(std::move(diagnostic));
}

std::string BuildReport::ToJson() const
{
    nlohmann::json json;
    json["schemaVersion"] = schemaVersion;
    json["succeeded"] = succeeded;
    json["profileId"] = profileId;
    json["profileName"] = profileName;
    json["buildMode"] = buildMode;
    json["targetId"] = targetId;
    json["artifactPlatform"] = artifactPlatform;
    json["productName"] = productName;
    json["productVersion"] = productVersion;
    json["buildNumber"] = buildNumber;
    json["engineVersion"] = engineVersion;
    json["playerId"] = playerId;
    json["playerTemplatePath"] = playerTemplatePath;
    json["configuration"] = configuration;
    json["inputHash"] = inputHash;
    json["startedUtc"] = startedUtc;
    json["finishedUtc"] = finishedUtc;
    json["outputDirectory"] = outputDirectory;
    json["packageDirectory"] = packageDirectory;
    json["signed"] = signed_;
    json["signingDescription"] = signingDescription;

    nlohmann::json phases = nlohmann::json::array();
    for (const auto& phase : this->phases)
        phases.push_back({{"phase", phase.phase}, {"seconds", phase.seconds}});
    json["phases"] = std::move(phases);

    nlohmann::json diagnosticsJson = nlohmann::json::array();
    for (const auto& diagnostic : diagnostics)
    {
        diagnosticsJson.push_back({
            {"severity", SeverityText(diagnostic.severity)},
            {"code", diagnostic.code},
            {"message", diagnostic.message},
            {"detail", diagnostic.detail}});
    }
    json["diagnostics"] = std::move(diagnosticsJson);

    nlohmann::json assetsJson = nlohmann::json::array();
    for (const auto& asset : includedAssets)
    {
        assetsJson.push_back({
            {"reference", asset.reference},
            {"sourcePath", asset.sourcePath},
            {"artifactPath", asset.artifactPath},
            {"package", asset.package},
            {"reason", asset.reason},
            {"size", asset.size},
            {"deduplicated", asset.deduplicated}});
    }
    json["includedAssets"] = std::move(assetsJson);

    nlohmann::json outputsJson = nlohmann::json::array();
    for (const auto& output : outputs)
    {
        outputsJson.push_back({
            {"path", output.path},
            {"kind", output.kind},
            {"size", output.size},
            {"sha256", output.sha256}});
    }
    json["outputs"] = std::move(outputsJson);

    nlohmann::json packagesJson = nlohmann::json::array();
    for (const auto& package : contentPackages)
        packagesJson.push_back(package);
    json["contentPackages"] = std::move(packagesJson);

    nlohmann::json cachedJson = nlohmann::json::array();
    for (const auto& stage : cachedStages)
        cachedJson.push_back(stage);
    json["cachedStages"] = std::move(cachedJson);

    return json.dump(2);
}

std::optional<BuildReport> BuildReport::FromJson(const std::string& json, std::string* error)
{
    try
    {
        const auto parsed = nlohmann::json::parse(json);
        BuildReport report;
        report.schemaVersion = parsed.value("schemaVersion", std::string("1"));
        report.succeeded = parsed.value("succeeded", false);
        report.profileId = parsed.value("profileId", std::string {});
        report.profileName = parsed.value("profileName", std::string {});
        report.buildMode = parsed.value("buildMode", std::string {});
        report.targetId = parsed.value("targetId", std::string {});
        report.artifactPlatform = parsed.value("artifactPlatform", std::string {});
        report.productName = parsed.value("productName", std::string {});
        report.productVersion = parsed.value("productVersion", std::string {});
        report.buildNumber = parsed.value("buildNumber", std::string {});
        report.engineVersion = parsed.value("engineVersion", std::string {});
        report.playerId = parsed.value("playerId", std::string {});
        report.playerTemplatePath = parsed.value("playerTemplatePath", std::string {});
        report.configuration = parsed.value("configuration", std::string {});
        report.inputHash = parsed.value("inputHash", std::string {});
        report.startedUtc = parsed.value("startedUtc", std::string {});
        report.finishedUtc = parsed.value("finishedUtc", std::string {});
        report.outputDirectory = parsed.value("outputDirectory", std::string {});
        report.packageDirectory = parsed.value("packageDirectory", std::string {});
        report.signed_ = parsed.value("signed", false);
        report.signingDescription = parsed.value("signingDescription", std::string {});

        if (parsed.contains("phases") && parsed["phases"].is_array())
        {
            for (const auto& phase : parsed["phases"])
            {
                BuildPhaseTiming timing;
                timing.phase = phase.value("phase", std::string {});
                timing.seconds = phase.value("seconds", 0.0);
                report.phases.push_back(std::move(timing));
            }
        }
        if (parsed.contains("diagnostics") && parsed["diagnostics"].is_array())
        {
            for (const auto& diagnostic : parsed["diagnostics"])
            {
                report.AddDiagnostic(
                    SeverityFromText(diagnostic.value("severity", std::string("info"))),
                    diagnostic.value("code", std::string {}),
                    diagnostic.value("message", std::string {}),
                    diagnostic.value("detail", std::string {}));
            }
        }
        if (parsed.contains("includedAssets") && parsed["includedAssets"].is_array())
        {
            for (const auto& asset : parsed["includedAssets"])
            {
                BuildIncludedAsset included;
                included.reference = asset.value("reference", std::string {});
                included.sourcePath = asset.value("sourcePath", std::string {});
                included.artifactPath = asset.value("artifactPath", std::string {});
                included.package = asset.value("package", std::string {});
                included.reason = asset.value("reason", std::string {});
                included.size = asset.value("size", 0ull);
                included.deduplicated = asset.value("deduplicated", false);
                report.includedAssets.push_back(std::move(included));
            }
        }
        if (parsed.contains("outputs") && parsed["outputs"].is_array())
        {
            for (const auto& output : parsed["outputs"])
            {
                BuildOutputFile file;
                file.path = output.value("path", std::string {});
                file.kind = output.value("kind", std::string {});
                file.size = output.value("size", 0ull);
                file.sha256 = output.value("sha256", std::string {});
                report.outputs.push_back(std::move(file));
            }
        }
        if (parsed.contains("contentPackages") && parsed["contentPackages"].is_array())
        {
            for (const auto& package : parsed["contentPackages"])
                report.contentPackages.push_back(package.get<std::string>());
        }
        if (parsed.contains("cachedStages") && parsed["cachedStages"].is_array())
        {
            for (const auto& stage : parsed["cachedStages"])
                report.cachedStages.push_back(stage.get<std::string>());
        }
        return report;
    }
    catch (const std::exception& exception)
    {
        if (error != nullptr)
            *error = exception.what();
        return std::nullopt;
    }
}

bool BuildReport::Save(const std::filesystem::path& path, std::string* error) const
{
    std::error_code directoryError;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), directoryError);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
            *error = "cannot write build report '" + path.string() + "'";
        return false;
    }
    output << ToJson();
    return output.good();
}

std::optional<BuildReport> BuildReport::Load(const std::filesystem::path& path, std::string* error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
            *error = "cannot read build report '" + path.string() + "'";
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    return FromJson(buffer.str(), error);
}

std::string BuildReport::FormatSummary() const
{
    std::ostringstream stream;
    stream << (succeeded ? "BUILD SUCCEEDED" : "BUILD FAILED")
           << " profile=" << profileId
           << " target=" << targetId
           << " mode=" << buildMode;
    if (!productName.empty())
        stream << " product=" << productName << " " << productVersion;
    stream << "\n";
    for (const auto& phase : phases)
        stream << "  phase " << phase.phase << ": " << std::fixed << std::setprecision(3) << phase.seconds << "s\n";
    for (const auto& diagnostic : diagnostics)
        stream << "  [" << SeverityText(diagnostic.severity) << "] " << diagnostic.code << ": "
               << diagnostic.message << (diagnostic.detail.empty() ? "" : (" (" + diagnostic.detail + ")")) << "\n";
    for (const auto& output : outputs)
        stream << "  output " << output.kind << ": " << output.path << " (" << output.size << " bytes, "
               << output.sha256.substr(0u, 16u) << ")\n";
    return stream.str();
}

std::string ComputeFileSha256(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};
    NLS::Core::Assets::Sha256Hasher hasher;
    std::vector<uint8_t> buffer(256u * 1024u);
    while (stream)
    {
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto read = stream.gcount();
        if (read > 0 && !hasher.Update(buffer.data(), static_cast<size_t>(read)))
            return {};
    }
    if (stream.bad())
        return {};
    return NLS::Core::Assets::ToHexString(hasher.Finalize());
}

std::string ComputeStringSha256(const std::string& text)
{
    return NLS::Core::Assets::ToHexString(NLS::Core::Assets::ComputeSha256(
        reinterpret_cast<const uint8_t*>(text.data()),
        text.size()));
}

std::string CurrentUtcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc {};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}
} // namespace NLS::Build
