#include "PackageInspection.h"

#include "BuildFileUtils.h"
#include "BuildReport.h"

#include "Assets/NpakArchive.h"
#include "Engine/Assets/PlayerDescriptor.h"
#include "Engine/Assets/RuntimeManifestSerialization.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace NLS::Build
{
namespace
{
void AddIssue(
    PackageInspection& inspection,
    std::string code,
    std::string message,
    std::string path = {})
{
    inspection.issues.push_back({std::move(code), std::move(message), std::move(path)});
}

bool FileExists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

std::string ReadAllText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}
} // namespace

bool InspectPackage(
    const std::filesystem::path& packageDirectory,
    PackageInspection& inspection,
    const bool verifyPayloadHashes)
{
    inspection = {};
    inspection.packageDirectory = packageDirectory;

    std::error_code directoryError;
    if (!std::filesystem::is_directory(packageDirectory, directoryError))
    {
        AddIssue(inspection, "package-missing", "The package directory does not exist.", packageDirectory.string());
        return false;
    }

    // Player descriptor.
    std::string descriptorError;
    const auto descriptor = NLS::Engine::Assets::PlayerDescriptor::Load(
        packageDirectory / NLS::Engine::Assets::PlayerDescriptor::FileName(),
        &descriptorError);
    if (!descriptor.has_value())
    {
        AddIssue(
            inspection,
            "package-player-descriptor",
            "Player.info is missing or invalid: " + descriptorError,
            packageDirectory.string());
        return false;
    }
    inspection.hasPlayerDescriptor = true;
    inspection.playerId = descriptor->playerId;
    inspection.targetId = descriptor->targetId;
    inspection.artifactPlatform = descriptor->artifactPlatform;
    inspection.engineVersion = descriptor->engineVersion;
    inspection.buildMode = descriptor->buildMode;
    inspection.productName = descriptor->productName;
    inspection.productVersion = descriptor->productVersion;
    inspection.startScene = descriptor->startScene;
    inspection.contentPackages = descriptor->contentPackages;
    inspection.scriptBackends = descriptor->scriptBackends;

    // Runtime manifest.
    const auto manifestPath = packageDirectory / "Data" / "User" / "RuntimeAssetManifest.json";
    if (!FileExists(manifestPath))
    {
        AddIssue(
            inspection,
            "package-runtime-manifest",
            "The runtime asset manifest is missing.",
            manifestPath.string());
        return false;
    }
    inspection.hasRuntimeManifest = true;
    std::string manifestError;
    const auto manifest = NLS::Engine::Assets::DeserializeRuntimeAssetManifest(
        ReadAllText(manifestPath),
        &manifestError);
    if (!manifest.has_value())
    {
        AddIssue(
            inspection,
            "package-runtime-manifest-invalid",
            "The runtime asset manifest is invalid: " + manifestError,
            manifestPath.string());
        return false;
    }
    if (!manifest->targetPlatform.empty() &&
        !inspection.artifactPlatform.empty() &&
        manifest->targetPlatform != inspection.artifactPlatform)
    {
        AddIssue(
            inspection,
            "package-platform-mismatch",
            "The runtime manifest target platform does not match Player.info.",
            manifest->targetPlatform + " != " + inspection.artifactPlatform);
    }

    // Packages listed by the descriptor must exist and open.
    std::set<std::string> packagedEntries;
    for (const auto& package : descriptor->contentPackages)
    {
        const auto packagePath = packageDirectory / std::filesystem::path(package);
        if (!FileExists(packagePath))
        {
            AddIssue(
                inspection,
                "package-content-missing",
                "A content package listed by Player.info is missing.",
                packagePath.string());
            continue;
        }
        std::string openError;
        auto archive = NLS::Core::Assets::NpakArchive::Open(packagePath, &openError);
        if (!archive.has_value())
        {
            AddIssue(
                inspection,
                "package-content-invalid",
                "A content package could not be opened: " + openError,
                packagePath.string());
            continue;
        }
        inspection.totalContentBytes += archive->GetInfo().totalSize;
        for (const auto& entry : archive->GetEntries())
        {
            packagedEntries.insert(entry.path);
            if (verifyPayloadHashes)
            {
                std::string verifyError;
                if (!archive->VerifyPayload(entry.path, &verifyError))
                {
                    AddIssue(
                        inspection,
                        "package-content-corrupt",
                        "A content package payload failed verification: " + verifyError,
                        packagePath.string());
                }
            }
        }
    }

    if (packagedEntries.empty())
    {
        AddIssue(
            inspection,
            "package-content-empty",
            "No content packages with payloads were found.",
            packageDirectory.string());
    }

    // Every manifest artifact must resolve inside a package.
    for (const auto& entry : manifest->entries)
    {
        const auto portable = NLS::Core::Assets::TryMakePortableContentArtifactPath(entry.artifactPath);
        const auto key = portable.empty() ? entry.artifactPath : portable;
        if (packagedEntries.find(key) == packagedEntries.end())
        {
            AddIssue(
                inspection,
                "package-artifact-unresolved",
                "The runtime manifest references an artifact that is not in any package.",
                entry.artifactPath);
        }
    }

    // Raw assets.
    const auto rawAssets = packageDirectory / "Data" / "User" / "Assets";
    for (const auto& file : EnumerateFiles(rawAssets, true))
    {
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(file, sizeError);
        if (!sizeError)
            inspection.rawContentBytes += size;
    }
    inspection.fileCount = EnumerateFiles(packageDirectory, true).size();

    return inspection.issues.empty();
}

bool ValidateArchive(const std::filesystem::path& archivePath, PackageInspection& inspection)
{
    inspection = {};
    if (!FileExists(archivePath))
    {
        AddIssue(inspection, "package-archive-missing", "The archive does not exist.", archivePath.string());
        return false;
    }

    // Minimal ZIP reader: locate the end-of-central-directory record and walk
    // the central directory. Entries must share one top-level directory.
    std::ifstream input(archivePath, std::ios::binary | std::ios::ate);
    if (!input)
    {
        AddIssue(inspection, "package-archive-read", "The archive cannot be read.", archivePath.string());
        return false;
    }
    const auto archiveSize = static_cast<uint64_t>(input.tellg());
    if (archiveSize < 22u)
    {
        AddIssue(inspection, "package-archive-truncated", "The archive is too small to be a ZIP file.", archivePath.string());
        return false;
    }

    const auto searchBytes = static_cast<uint64_t>((std::min<uint64_t>)(archiveSize, 66u * 1024u));
    std::vector<uint8_t> tail(static_cast<size_t>(searchBytes));
    input.seekg(static_cast<std::streamoff>(archiveSize - searchBytes), std::ios::beg);
    input.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (!input)
    {
        AddIssue(inspection, "package-archive-read", "The archive tail cannot be read.", archivePath.string());
        return false;
    }

    constexpr uint32_t kEndSignature = 0x06054b50u;
    int64_t endOffset = -1;
    for (int64_t index = static_cast<int64_t>(tail.size()) - 22; index >= 0; --index)
    {
        const auto* bytes = tail.data() + index;
        const auto signature = static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8u) |
            (static_cast<uint32_t>(bytes[2]) << 16u) |
            (static_cast<uint32_t>(bytes[3]) << 24u);
        if (signature == kEndSignature)
        {
            endOffset = index;
            break;
        }
    }
    if (endOffset < 0)
    {
        AddIssue(inspection, "package-archive-invalid", "The archive has no ZIP end-of-directory record.", archivePath.string());
        return false;
    }

    const auto readU16 = [](const uint8_t* bytes)
    {
        return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8u);
    };
    const auto readU32 = [](const uint8_t* bytes)
    {
        return static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8u) |
            (static_cast<uint32_t>(bytes[2]) << 16u) |
            (static_cast<uint32_t>(bytes[3]) << 24u);
    };

    const auto* end = tail.data() + endOffset;
    const auto entryCount = readU16(end + 10u);
    const auto centralSize = readU32(end + 12u);
    const auto centralOffset = readU32(end + 16u);
    if (centralOffset + centralSize > archiveSize)
    {
        AddIssue(inspection, "package-archive-invalid", "The ZIP central directory is out of bounds.", archivePath.string());
        return false;
    }

    std::vector<uint8_t> central(centralSize);
    input.clear();
    input.seekg(static_cast<std::streamoff>(centralOffset), std::ios::beg);
    input.read(reinterpret_cast<char*>(central.data()), static_cast<std::streamsize>(central.size()));
    if (!input && !central.empty())
    {
        AddIssue(inspection, "package-archive-read", "The ZIP central directory cannot be read.", archivePath.string());
        return false;
    }

    constexpr uint32_t kCentralSignature = 0x02014b50u;
    size_t offset = 0u;
    std::set<std::string> topLevelDirectories;
    uint32_t entries = 0u;
    while (offset + 46u <= central.size() && entries < entryCount)
    {
        const auto* record = central.data() + offset;
        if (readU32(record) != kCentralSignature)
            break;
        const auto nameLength = readU16(record + 28u);
        const auto extraLength = readU16(record + 30u);
        const auto commentLength = readU16(record + 32u);
        if (offset + 46u + nameLength > central.size())
            break;
        const std::string name(
            reinterpret_cast<const char*>(record + 46u),
            static_cast<size_t>(nameLength));
        const auto slash = name.find('/');
        if (slash == std::string::npos)
        {
            AddIssue(
                inspection,
                "package-archive-layout",
                "The archive contains files outside the product directory.",
                name);
        }
        else
        {
            topLevelDirectories.insert(name.substr(0u, slash));
        }
        offset += 46u + nameLength + extraLength + commentLength;
        ++entries;
    }
    if (topLevelDirectories.size() != 1u)
    {
        AddIssue(
            inspection,
            "package-archive-layout",
            "The archive must contain exactly one top-level product directory.",
            std::to_string(topLevelDirectories.size()) + " found");
        return false;
    }
    const auto productDirectoryName = *topLevelDirectories.begin();

    // Extract to a temporary directory next to the archive, inspect, then
    // clean up. This keeps validation independent of any project state.
    std::error_code error;
    const auto temporaryRoot = std::filesystem::temp_directory_path(error) /
        ("nullus-validate-" + ComputeStringSha256(archivePath.string()).substr(0u, 12u));
    RemoveDirectoryIfExists(temporaryRoot);
    if (!EnsureDirectory(temporaryRoot))
    {
        AddIssue(inspection, "package-archive-extract", "Cannot create the validation directory.", temporaryRoot.string());
        return false;
    }

    // Use PowerShell's Expand-Archive on Windows; the tool is only used by
    // validation, never by the packaging pipeline itself.
    std::ostringstream command;
#if defined(_WIN32)
    command << "powershell -NoProfile -NonInteractive -Command \"Expand-Archive -LiteralPath '"
            << archivePath.string() << "' -DestinationPath '" << temporaryRoot.string() << "' -Force\"";
#else
    command << "unzip -o -q '" << archivePath.string() << "' -d '" << temporaryRoot.string() << "'";
#endif
    const auto result = std::system(command.str().c_str());
    if (result != 0)
    {
        AddIssue(inspection, "package-archive-extract", "The archive could not be extracted.", archivePath.string());
        RemoveDirectoryIfExists(temporaryRoot);
        return false;
    }

    const bool valid = InspectPackage(temporaryRoot / productDirectoryName, inspection, true);
    inspection.packageDirectory = archivePath;
    RemoveDirectoryIfExists(temporaryRoot);
    return valid;
}

std::string FormatInspection(const PackageInspection& inspection)
{
    std::ostringstream stream;
    stream << "Package: " << inspection.packageDirectory.string() << "\n";
    stream << "  product: " << inspection.productName << " " << inspection.productVersion << "\n";
    stream << "  player: " << inspection.playerId << "\n";
    stream << "  target: " << inspection.targetId << " (" << inspection.artifactPlatform << ")\n";
    stream << "  mode: " << inspection.buildMode << ", engine: " << inspection.engineVersion << "\n";
    stream << "  start scene: " << inspection.startScene << "\n";
    stream << "  packages: " << inspection.contentPackages.size()
           << ", files: " << inspection.fileCount
           << ", content bytes: " << inspection.totalContentBytes
           << ", raw bytes: " << inspection.rawContentBytes << "\n";
    if (inspection.issues.empty())
    {
        stream << "  result: valid\n";
    }
    else
    {
        stream << "  result: " << inspection.issues.size() << " issue(s)\n";
        for (const auto& issue : inspection.issues)
            stream << "    [" << issue.code << "] " << issue.message
                   << (issue.path.empty() ? "" : (" (" + issue.path + ")")) << "\n";
    }
    return stream.str();
}
} // namespace NLS::Build
