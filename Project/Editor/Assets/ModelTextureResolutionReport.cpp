#include "Assets/ModelTextureResolutionReport.h"

#include "Assets/ModelTextureTextCodec.h"
#include "Guid.h"

#include <limits>
#include <map>
#include <string_view>

namespace NLS::Editor::Assets
{
namespace
{
std::optional<uint32_t> ParseUInt32(std::string_view text)
{
    if (text.empty())
        return std::nullopt;

    uint32_t value = 0u;
    for (const char character : text)
    {
        if (character < '0' || character > '9')
            return std::nullopt;

        const uint32_t digit = static_cast<uint32_t>(character - '0');
        if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10u)
            return std::nullopt;

        value = value * 10u + digit;
    }
    return value;
}

std::optional<ModelTextureResolutionKind> ParseResolutionKind(const std::string& value)
{
    if (value == "ExplicitRemap")
        return ModelTextureResolutionKind::ExplicitRemap;
    if (value == "SourcePath")
        return ModelTextureResolutionKind::SourcePath;
    if (value == "NameSearch")
        return ModelTextureResolutionKind::NameSearch;
    if (value == "ModelEmbeddedFallback")
        return ModelTextureResolutionKind::ModelEmbeddedFallback;
    if (value == "Missing")
        return ModelTextureResolutionKind::Missing;
    if (value == "Invalid")
        return ModelTextureResolutionKind::Invalid;
    return std::nullopt;
}

std::optional<TextureSourceKind> ParseTextureSourceKind(const std::string& value)
{
    if (value == "ExternalFile")
        return TextureSourceKind::ExternalFile;
    if (value == "EmbeddedData")
        return TextureSourceKind::EmbeddedData;
    if (value == "BufferView")
        return TextureSourceKind::BufferView;
    if (value == "Missing")
        return TextureSourceKind::Missing;
    return std::nullopt;
}

std::optional<ModelTextureStableKeyStatus> ParseStableKeyStatus(const std::string& value)
{
    if (value == "Stable")
        return ModelTextureStableKeyStatus::Stable;
    if (value == "OrderDerived")
        return ModelTextureStableKeyStatus::OrderDerived;
    if (value == "Insufficient")
        return ModelTextureStableKeyStatus::Insufficient;
    return std::nullopt;
}

void AppendLine(std::string& text, const std::string& key, const std::string& value)
{
    text += key;
    text.push_back('=');
    text += EncodeModelTextureTextField(value);
    text.push_back('\n');
}

std::optional<std::string> GetDecoded(
    const std::map<std::string, std::string>& values,
    const std::string& key)
{
    const auto found = values.find(key);
    if (found == values.end())
        return std::string {};
    return DecodeModelTextureTextField(found->second);
}

std::optional<uint32_t> GetUInt32(
    const std::map<std::string, std::string>& values,
    const std::string& key)
{
    const auto decoded = GetDecoded(values, key);
    if (!decoded.has_value())
        return std::nullopt;
    return ParseUInt32(*decoded);
}
}

std::string SerializeModelTextureResolutionReport(const ModelTextureResolutionReport& report)
{
    std::string text = "NULLUS_MODEL_TEXTURE_RESOLUTION_REPORT=1\n";
    text += "reportVersion=" + std::to_string(report.reportVersion) + "\n";
    AppendLine(text, "modelAssetId", report.modelAssetId);
    AppendLine(text, "targetPlatform", report.targetPlatform);
    text += "importerVersion=" + std::to_string(report.importerVersion) + "\n";
    AppendLine(text, "settingsFingerprint", report.settingsFingerprint);
    text += "entryCount=" + std::to_string(report.entries.size()) + "\n";

    for (size_t entryIndex = 0u; entryIndex < report.entries.size(); ++entryIndex)
    {
        const auto& entry = report.entries[entryIndex];
        const std::string prefix = "entry." + std::to_string(entryIndex) + ".";
        AppendLine(text, prefix + "sourceStableKey", entry.source.stableKey);
        AppendLine(text, prefix + "sourceKey", entry.source.sourceKey);
        AppendLine(text, prefix + "materialTextureKey", entry.source.materialTextureKey);
        AppendLine(text, prefix + "displayName", entry.source.displayName);
        AppendLine(text, prefix + "uri", entry.source.uri);
        AppendLine(text, prefix + "normalizedUri", entry.source.normalizedUri);
        AppendLine(text, prefix + "textureSourceKind", ToString(entry.source.kind));
        AppendLine(text, prefix + "stableKeyStatus", ToString(entry.source.stableKeyStatus));
        AppendLine(text, prefix + "resolutionKind", ToString(entry.kind));
        AppendLine(
            text,
            prefix + "targetAssetId",
            entry.targetAssetId.IsValid() ? entry.targetAssetId.ToString() : std::string {});
        AppendLine(text, prefix + "targetSubAssetKey", entry.targetSubAssetKey);
        AppendLine(text, prefix + "targetEditorPath", entry.targetEditorPath.generic_string());
        AppendLine(text, prefix + "resourcePath", entry.resourcePath.generic_string());
        AppendLine(text, prefix + "modelSubAssetKey", entry.modelSubAssetKey);
        text += prefix + "diagnosticCount=" + std::to_string(entry.diagnostics.size()) + "\n";

        for (size_t diagnosticIndex = 0u; diagnosticIndex < entry.diagnostics.size(); ++diagnosticIndex)
        {
            const auto& diagnostic = entry.diagnostics[diagnosticIndex];
            const std::string diagnosticPrefix =
                prefix + "diagnostic." + std::to_string(diagnosticIndex) + ".";
            AppendLine(text, diagnosticPrefix + "severity", diagnostic.severity);
            AppendLine(text, diagnosticPrefix + "code", diagnostic.code);
            AppendLine(text, diagnosticPrefix + "message", diagnostic.message);
        }
    }

    return text;
}

std::optional<ModelTextureResolutionReport> ParseModelTextureResolutionReport(const std::string& text)
{
    std::map<std::string, std::string> values;
    size_t lineStart = 0u;
    while (lineStart <= text.size())
    {
        const size_t lineEnd = text.find('\n', lineStart);
        const auto line = lineEnd == std::string::npos ?
            std::string_view(text).substr(lineStart) :
            std::string_view(text).substr(lineStart, lineEnd - lineStart);

        if (!line.empty())
        {
            const size_t separator = line.find('=');
            if (separator == std::string_view::npos)
                return std::nullopt;
            values.emplace(
                std::string(line.substr(0u, separator)),
                std::string(line.substr(separator + 1u)));
        }

        if (lineEnd == std::string::npos)
            break;
        lineStart = lineEnd + 1u;
    }

    const auto magic = values.find("NULLUS_MODEL_TEXTURE_RESOLUTION_REPORT");
    if (magic == values.end() || magic->second != "1")
        return std::nullopt;

    ModelTextureResolutionReport report;

    const auto reportVersion = GetUInt32(values, "reportVersion");
    const auto importerVersion = GetUInt32(values, "importerVersion");
    const auto entryCount = GetUInt32(values, "entryCount");
    if (!reportVersion.has_value() || !importerVersion.has_value() || !entryCount.has_value())
        return std::nullopt;

    report.reportVersion = *reportVersion;
    report.importerVersion = *importerVersion;

    auto modelAssetId = GetDecoded(values, "modelAssetId");
    auto targetPlatform = GetDecoded(values, "targetPlatform");
    auto settingsFingerprint = GetDecoded(values, "settingsFingerprint");
    if (!modelAssetId.has_value() || !targetPlatform.has_value() || !settingsFingerprint.has_value())
        return std::nullopt;
    report.modelAssetId = *modelAssetId;
    report.targetPlatform = *targetPlatform;
    report.settingsFingerprint = *settingsFingerprint;

    for (uint32_t entryIndex = 0u; entryIndex < *entryCount; ++entryIndex)
    {
        const std::string prefix = "entry." + std::to_string(entryIndex) + ".";

        ResolvedModelTextureReference entry;
        auto sourceStableKey = GetDecoded(values, prefix + "sourceStableKey");
        auto sourceKey = GetDecoded(values, prefix + "sourceKey");
        auto materialTextureKey = GetDecoded(values, prefix + "materialTextureKey");
        auto displayName = GetDecoded(values, prefix + "displayName");
        auto uri = GetDecoded(values, prefix + "uri");
        auto normalizedUri = GetDecoded(values, prefix + "normalizedUri");
        auto textureSourceKind = GetDecoded(values, prefix + "textureSourceKind");
        auto stableKeyStatus = GetDecoded(values, prefix + "stableKeyStatus");
        auto resolutionKind = GetDecoded(values, prefix + "resolutionKind");
        auto targetAssetId = GetDecoded(values, prefix + "targetAssetId");
        auto targetSubAssetKey = GetDecoded(values, prefix + "targetSubAssetKey");
        auto targetEditorPath = GetDecoded(values, prefix + "targetEditorPath");
        auto resourcePath = GetDecoded(values, prefix + "resourcePath");
        auto modelSubAssetKey = GetDecoded(values, prefix + "modelSubAssetKey");
        const auto diagnosticCount = GetUInt32(values, prefix + "diagnosticCount");

        if (!sourceStableKey.has_value() ||
            !sourceKey.has_value() ||
            !materialTextureKey.has_value() ||
            !displayName.has_value() ||
            !uri.has_value() ||
            !normalizedUri.has_value() ||
            !textureSourceKind.has_value() ||
            !stableKeyStatus.has_value() ||
            !resolutionKind.has_value() ||
            !targetAssetId.has_value() ||
            !targetSubAssetKey.has_value() ||
            !targetEditorPath.has_value() ||
            !resourcePath.has_value() ||
            !modelSubAssetKey.has_value() ||
            !diagnosticCount.has_value())
        {
            return std::nullopt;
        }

        const auto parsedTextureSourceKind = ParseTextureSourceKind(*textureSourceKind);
        const auto parsedStableKeyStatus = ParseStableKeyStatus(*stableKeyStatus);
        const auto parsedResolutionKind = ParseResolutionKind(*resolutionKind);
        if (!parsedTextureSourceKind.has_value() ||
            !parsedStableKeyStatus.has_value() ||
            !parsedResolutionKind.has_value())
        {
            return std::nullopt;
        }

        entry.source.stableKey = *sourceStableKey;
        entry.source.sourceKey = *sourceKey;
        entry.source.materialTextureKey = *materialTextureKey;
        entry.source.displayName = *displayName;
        entry.source.uri = *uri;
        entry.source.normalizedUri = *normalizedUri;
        entry.source.kind = *parsedTextureSourceKind;
        entry.source.stableKeyStatus = *parsedStableKeyStatus;
        entry.kind = *parsedResolutionKind;
        entry.materialTextureKey = *materialTextureKey;
        entry.targetSubAssetKey = *targetSubAssetKey;
        entry.targetEditorPath = *targetEditorPath;
        entry.resourcePath = *resourcePath;
        entry.modelSubAssetKey = *modelSubAssetKey;

        if (!targetAssetId->empty())
        {
            const auto guid = NLS::Guid::TryParse(*targetAssetId);
            if (!guid.has_value())
                return std::nullopt;
            entry.targetAssetId = NLS::Core::Assets::AssetId(*guid);
        }

        for (uint32_t diagnosticIndex = 0u; diagnosticIndex < *diagnosticCount; ++diagnosticIndex)
        {
            const std::string diagnosticPrefix =
                prefix + "diagnostic." + std::to_string(diagnosticIndex) + ".";
            auto severity = GetDecoded(values, diagnosticPrefix + "severity");
            auto code = GetDecoded(values, diagnosticPrefix + "code");
            auto message = GetDecoded(values, diagnosticPrefix + "message");
            if (!severity.has_value() || !code.has_value() || !message.has_value())
                return std::nullopt;
            entry.diagnostics.push_back({*severity, *code, *message});
        }

        report.entries.push_back(std::move(entry));
    }

    return report;
}

bool IsModelTextureResolutionReportCurrent(
    const ModelTextureResolutionReport& report,
    const ModelTextureReportContext& context)
{
    return report.modelAssetId == context.modelAssetId &&
        report.targetPlatform == context.targetPlatform &&
        report.importerVersion == context.importerVersion &&
        report.settingsFingerprint == context.settingsFingerprint &&
        report.reportVersion == 1u;
}

std::filesystem::path ModelTextureResolutionReportPath(const std::filesystem::path& committedArtifactRoot)
{
    return committedArtifactRoot / "texture-resolution-report.txt";
}
}
