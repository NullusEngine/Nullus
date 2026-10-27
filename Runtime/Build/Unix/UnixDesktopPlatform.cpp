#include "Unix/UnixDesktopPlatform.h"

#include "ArchiveWriter.h"
#include "BuildFileUtils.h"
#include "BuildPipeline.h"
#include "BuildReport.h"

#include "Engine/Assets/PlayerDescriptor.h"
#include "Platform/Process/Process.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace NLS::Build
{
namespace
{
std::string SanitizeName(std::string value)
{
    for (auto& character : value)
    {
        const auto invalid = character == '<' || character == '>' || character == ':' ||
            character == '"' || character == '/' || character == '\\' || character == '|' ||
            character == '?' || character == '*';
        if (invalid || static_cast<unsigned char>(character) < 0x20u)
            character = '-';
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '.'))
        value.pop_back();
    return value.empty() ? std::string("NullusGame") : value;
}

std::string ExecutableName(const BuildProfile& profile)
{
    const auto base = profile.executableName.empty()
        ? (profile.productName.empty() ? std::string("NullusGame") : profile.productName)
        : profile.executableName;
    return SanitizeName(base);
}

std::string MakeOutputDirectoryName(const BuildProfile& profile, const BuildTarget& target)
{
    std::string name = SanitizeName(
        profile.productName.empty() ? profile.executableName : profile.productName);
    name += "-";
    name += profile.version.empty() ? "0.0.0" : profile.version;
    if (!profile.buildNumber.empty())
    {
        name += "-";
        name += SanitizeName(profile.buildNumber);
    }
    name += "-";
    name += BuildTargetId(target);
    return name;
}

std::string NormalizeSlashes(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

bool HostMatches(const PlatformFamily family)
{
#if defined(__linux__)
    return family == PlatformFamily::Linux;
#elif defined(__APPLE__)
    return family == PlatformFamily::MacOS;
#else
    (void)family;
    return false;
#endif
}

std::vector<std::filesystem::path> TemplateRoots(
    const BuildContext& context,
    const PlatformFamily family)
{
    std::vector<std::filesystem::path> roots;
    const auto append = [&roots](const std::filesystem::path& path)
    {
        if (!path.empty())
            roots.push_back(path);
    };
    if (!context.playerTemplatePath.empty())
        append(context.playerTemplatePath);
    if (!context.profile.playerTemplatePath.empty())
    {
        std::filesystem::path configured(context.profile.playerTemplatePath);
        append(configured.is_absolute() ? configured : context.projectRoot / configured);
    }
    if (const char* environment = std::getenv("NLS_PLAYER_TEMPLATE");
        environment != nullptr && environment[0] != '\0')
    {
        append(environment);
    }
    if (!context.editorExecutablePath.empty())
        append(context.editorExecutablePath.parent_path());

    const auto platformName = family == PlatformFamily::Linux ? "Linux" : "MacOS";
    const auto configuration = BuildModeConfiguration(context.profile.mode);
    std::vector<std::string> configurations {configuration};
    if (configuration != "Release")
        configurations.push_back("Release");
    if (configuration != "RelWithDebInfo")
        configurations.push_back("RelWithDebInfo");
    if (configuration != "Debug")
        configurations.push_back("Debug");

    std::vector<std::filesystem::path> appRoots;
    const auto resourceRoots = NLS::Platform::Process::ResolveInstallResourceRoots();
    appRoots.push_back(resourceRoots.installRoot / "App");
    appRoots.push_back(std::filesystem::current_path() / "App");
    if (!context.editorExecutablePath.empty())
    {
        for (auto probe = context.editorExecutablePath.parent_path();
             !probe.empty();
             probe = probe.parent_path())
        {
            appRoots.push_back(probe / "App");
            if (probe == probe.parent_path())
                break;
        }
    }
    for (const auto& appRoot : appRoots)
    {
        for (const auto& candidateConfiguration : configurations)
        {
            append(appRoot / (std::string(platformName) + "_" + candidateConfiguration + "_Runtime_Shared"));
            append(appRoot / (std::string(platformName) + "_" + candidateConfiguration + "_Shared"));
        }
    }
    return roots;
}

std::optional<std::filesystem::path> FindExecutable(
    const std::filesystem::path& root,
    const PlatformFamily family)
{
    std::error_code error;
    if (family == PlatformFamily::MacOS)
    {
        const auto bundle = root / "Game.app" / "Contents" / "MacOS" / "Game";
        if (std::filesystem::is_regular_file(bundle, error))
            return bundle;
    }
    for (const auto& name : {"Game", "Game.x86_64"})
    {
        const auto candidate = root / name;
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate;
    }
    return std::nullopt;
}

bool IsBundleExecutable(const std::filesystem::path& relative)
{
    const auto text = NormalizeSlashes(relative.generic_string());
    return text == "Game" || text == "Game.x86_64" ||
        text == "Game.app/Contents/MacOS/Game";
}

uint16_t ReadLe16(const std::vector<uint8_t>& bytes, const size_t offset)
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(bytes[offset]) |
        (static_cast<uint16_t>(bytes[offset + 1u]) << 8u));
}

uint32_t ReadLe32(const std::vector<uint8_t>& bytes, const size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

uint64_t ReadLe64(const std::vector<uint8_t>& bytes, const size_t offset)
{
    return static_cast<uint64_t>(ReadLe32(bytes, offset)) |
        (static_cast<uint64_t>(ReadLe32(bytes, offset + 4u)) << 32u);
}

bool PatchElfRunPath(const std::filesystem::path& path, std::string* error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return true;
    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        (std::istreambuf_iterator<char>()));
    input.close();

    if (bytes.size() < 52u || bytes[0] != 0x7Fu || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F')
    {
        return true;
    }
    const auto elfClass = bytes[4];
    const auto dataEncoding = bytes[5];
    if (dataEncoding != 1u || (elfClass != 1u && elfClass != 2u))
        return true;
    const bool elf64 = elfClass == 2u;

    const auto readWord = [&](const size_t offset) -> uint64_t
    {
        return elf64 ? ReadLe64(bytes, offset) : ReadLe32(bytes, offset);
    };
    const auto readHalf = [&](const size_t offset) -> uint16_t
    {
        return ReadLe16(bytes, offset);
    };

    const auto phoff = static_cast<size_t>(readWord(elf64 ? 32u : 28u));
    const auto phentsize = readHalf(elf64 ? 54u : 42u);
    const auto phnum = readHalf(elf64 ? 56u : 44u);
    if (phoff == 0u || phentsize < (elf64 ? 56u : 32u) || phnum == 0u ||
        phoff + static_cast<size_t>(phentsize) * phnum > bytes.size())
    {
        return true;
    }

    struct Segment
    {
        uint64_t offset = 0u;
        uint64_t virtualAddress = 0u;
        uint64_t fileSize = 0u;
    };
    std::vector<Segment> segments;
    size_t dynamicOffset = 0u;
    size_t dynamicSize = 0u;
    for (uint16_t index = 0u; index < phnum; ++index)
    {
        const auto header = phoff + static_cast<size_t>(phentsize) * index;
        const auto type = ReadLe32(bytes, header);
        if (type == 1u)
        {
            Segment segment;
            segment.offset = readWord(header + (elf64 ? 8u : 4u));
            segment.virtualAddress = readWord(header + (elf64 ? 16u : 8u));
            segment.fileSize = readWord(header + (elf64 ? 32u : 16u));
            segments.push_back(segment);
        }
        else if (type == 2u)
        {
            dynamicOffset = static_cast<size_t>(readWord(header + (elf64 ? 8u : 4u)));
            dynamicSize = static_cast<size_t>(readWord(header + (elf64 ? 32u : 16u)));
        }
    }
    if (dynamicOffset == 0u || dynamicSize == 0u ||
        dynamicOffset + dynamicSize > bytes.size())
    {
        return true;
    }

    const auto dynamicEntrySize = elf64 ? 16u : 8u;
    uint64_t stringTableAddress = 0u;
    uint64_t stringTableSize = 0u;
    std::vector<size_t> runPathOffsets;
    for (size_t offset = dynamicOffset;
         offset + dynamicEntrySize <= dynamicOffset + dynamicSize;
         offset += dynamicEntrySize)
    {
        const auto tag = readWord(offset);
        const auto value = readWord(offset + (elf64 ? 8u : 4u));
        if (tag == 5u)
            stringTableAddress = value;
        else if (tag == 10u)
            stringTableSize = value;
        else if (tag == 15u || tag == 29u)
            runPathOffsets.push_back(static_cast<size_t>(value));
    }
    if (stringTableAddress == 0u || stringTableSize == 0u || runPathOffsets.empty())
        return true;

    size_t stringTableOffset = 0u;
    bool mapped = false;
    for (const auto& segment : segments)
    {
        if (stringTableAddress >= segment.virtualAddress &&
            stringTableAddress < segment.virtualAddress + segment.fileSize)
        {
            stringTableOffset = static_cast<size_t>(
                segment.offset + (stringTableAddress - segment.virtualAddress));
            mapped = true;
            break;
        }
    }
    if (!mapped || stringTableOffset + stringTableSize > bytes.size())
        return true;

    size_t patched = 0u;
    for (const auto offset : runPathOffsets)
    {
        if (offset >= stringTableSize)
            continue;
        const auto textOffset = stringTableOffset + offset;
        const auto end = std::find(
            bytes.begin() + static_cast<std::ptrdiff_t>(textOffset),
            bytes.begin() + static_cast<std::ptrdiff_t>(
                std::min(bytes.size(), stringTableOffset + static_cast<size_t>(stringTableSize))),
            0u);
        if (end == bytes.end())
            continue;
        const std::string current(
            bytes.begin() + static_cast<std::ptrdiff_t>(textOffset),
            end);
        if (current == "$ORIGIN" || current.empty())
            continue;
        const std::string replacement = "$ORIGIN";
        if (current.size() < replacement.size())
            continue;
        std::copy(replacement.begin(), replacement.end(), bytes.begin() + static_cast<std::ptrdiff_t>(textOffset));
        std::fill(
            bytes.begin() + static_cast<std::ptrdiff_t>(textOffset + replacement.size()),
            end,
            0u);
        ++patched;
    }
    if (patched == 0u)
        return true;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
            *error = "cannot rewrite ELF runpath in '" + path.string() + "'";
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output)
    {
        if (error != nullptr)
            *error = "cannot write ELF runpath in '" + path.string() + "'";
        return false;
    }
    return true;
}

bool PatchMacBundleExecutable(
    const std::filesystem::path& bundleDirectory,
    const std::string& executableName,
    std::string* error)
{
    const auto plist = bundleDirectory / "Contents" / "Info.plist";
    std::ifstream input(plist, std::ios::binary);
    if (!input)
    {
        if (error != nullptr)
            *error = "macOS bundle has no Info.plist: " + plist.string();
        return false;
    }
    std::string text(
        (std::istreambuf_iterator<char>(input)),
        (std::istreambuf_iterator<char>()));
    input.close();

    const auto key = text.find("<key>CFBundleExecutable</key>");
    if (key == std::string::npos)
    {
        if (error != nullptr)
            *error = "macOS bundle Info.plist has no CFBundleExecutable key";
        return false;
    }
    const auto valueStart = text.find("<string>", key);
    const auto valueEnd = valueStart == std::string::npos
        ? std::string::npos
        : text.find("</string>", valueStart + 8u);
    if (valueStart == std::string::npos || valueEnd == std::string::npos)
    {
        if (error != nullptr)
            *error = "macOS bundle Info.plist has a malformed CFBundleExecutable value";
        return false;
    }

    std::string escaped;
    for (const auto character : executableName)
    {
        if (character == '&')
            escaped += "&amp;";
        else if (character == '<')
            escaped += "&lt;";
        else if (character == '>')
            escaped += "&gt;";
        else
            escaped.push_back(character);
    }
    text.replace(valueStart + 8u, valueEnd - valueStart - 8u, escaped);
    std::ofstream output(plist, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (error != nullptr)
            *error = "cannot rewrite macOS bundle Info.plist: " + plist.string();
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}
} // namespace

UnixDesktopPlatform::UnixDesktopPlatform(const PlatformFamily family)
    : m_family(family)
{
}

std::string UnixDesktopPlatform::GetAdapterId() const
{
    return m_family == PlatformFamily::Linux
        ? "linux-x64-desktop"
        : "macos-desktop";
}

PlatformCapability UnixDesktopPlatform::GetCapability(const BuildTarget& target) const
{
    PlatformCapability capability;
    capability.platform = m_family;
    capability.architecture = target.architecture;
    capability.graphicsBackend = target.graphicsBackend;
    capability.id = std::string(ToString(m_family)) + "-" +
        ToString(target.architecture) + "-" + ToString(target.graphicsBackend);

    if (target.platform != m_family)
    {
        capability.reason = "The adapter only builds its own desktop platform.";
        return capability;
    }
    if (m_family == PlatformFamily::Linux &&
        target.graphicsBackend != GraphicsBackendKind::Vulkan &&
        target.graphicsBackend != GraphicsBackendKind::OpenGL)
    {
        capability.reason = "Linux desktop supports Vulkan and OpenGL.";
        return capability;
    }
    if (m_family == PlatformFamily::MacOS &&
        target.graphicsBackend != GraphicsBackendKind::Metal)
    {
        capability.reason = "macOS desktop requires Metal.";
        return capability;
    }
    if (m_family == PlatformFamily::Linux && target.architecture != Architecture::X64)
    {
        capability.reason = "The Linux desktop adapter currently builds x64 players.";
        return capability;
    }
    if (m_family == PlatformFamily::MacOS &&
        target.architecture != Architecture::X64 &&
        target.architecture != Architecture::Arm64)
    {
        capability.reason = "macOS desktop requires x64 or arm64.";
        return capability;
    }

    capability.adapterAvailable = true;
    // Linux templates can be assembled on another host (for example a Windows
    // designer building a Linux payload); only the final self-test is
    // host-dependent. macOS needs a Mach-O toolchain and codesign, so its
    // adapter remains tied to a macOS host.
    capability.toolchainAvailable = m_family == PlatformFamily::Linux || HostMatches(m_family);
    if (!capability.toolchainAvailable)
    {
        capability.reason = "macOS packaging requires a macOS host for Mach-O validation and codesigning.";
    }
    return capability;
}

bool UnixDesktopPlatform::CheckToolchain(
    const BuildContext& context,
    BuildReport& report,
    std::string* error)
{
    (void)report;
    if (m_family == PlatformFamily::MacOS && !HostMatches(PlatformFamily::MacOS))
    {
        if (error != nullptr)
            *error = "macOS packaging requires a macOS host for Mach-O validation and codesigning.";
        return false;
    }
    const auto candidate = TemplateRoots(context, m_family);
    for (const auto& root : candidate)
    {
        std::error_code rootError;
        if (std::filesystem::is_directory(root, rootError) &&
            FindExecutable(root, m_family).has_value())
        {
            return true;
        }
    }
    if (error != nullptr)
    {
        *error = "No " + std::string(ToString(m_family)) +
            " player template containing Game was found. Build the Game target or pass --player.";
    }
    return false;
}

std::optional<PlayerTemplateInfo> UnixDesktopPlatform::ResolvePlayerTemplate(
    const BuildContext& context,
    BuildReport& report,
    std::string* error)
{
    for (const auto& candidate : TemplateRoots(context, m_family))
    {
        std::error_code rootError;
        if (!std::filesystem::is_directory(candidate, rootError))
            continue;
        const auto executable = FindExecutable(candidate, m_family);
        if (!executable.has_value())
            continue;

        PlayerTemplateInfo info;
        info.directory = candidate;
        info.executablePath = *executable;
        info.executableName = ExecutableName(context.profile);
        info.sourceBuilt = !context.profile.customNativeModules.empty();

        const auto expectedConfiguration = BuildModeConfiguration(context.profile.mode);
        const auto candidateText = candidate.string();
        if (expectedConfiguration != "Debug" &&
            candidateText.find("_Debug") != std::string::npos)
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Warning,
                "build-player-template-configuration",
                "The selected player template looks like a Debug template for a non-Debug build; "
                "verify that this is intentional.",
                candidate.string());
        }

        if (const auto descriptor = NLS::Engine::Assets::PlayerDescriptor::Load(
                candidate / "Player.info", nullptr);
            descriptor.has_value())
        {
            info.engineVersion = descriptor->engineVersion;
            info.playerId = descriptor->playerId;
            info.targetId = descriptor->targetId;
        }

        for (const auto& file : EnumerateFiles(candidate, true))
        {
            std::error_code relativeError;
            const auto relative = std::filesystem::relative(file, candidate, relativeError);
            if (relativeError || relative.empty() || !IsRuntimeTemplateFile(relative))
                continue;
            info.runtimeFiles.push_back(relative);
            const auto text = NormalizeSlashes(relative.generic_string());
            if (text.rfind("Scripts/", 0u) == 0u)
                info.hasManagedAssemblies = true;
            if (text.rfind("Assets/Scripts/", 0u) == 0u)
                info.hasLuaScripts = true;
        }

        if (info.runtimeFiles.empty())
        {
            if (error != nullptr)
                *error = "player template '" + candidate.string() + "' contains no runtime files";
            return std::nullopt;
        }
        if (info.targetId.empty())
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Warning,
                "build-player-template-descriptor",
                "The Unix player template has no Player.info; target compatibility relies on the profile.",
                candidate.string());
        }
        if (info.engineVersion.empty())
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Warning,
                "build-player-template-version",
                "The Unix player template does not record an engine version.",
                candidate.string());
        }
        return info;
    }

    if (error != nullptr)
    {
        *error = "no " + std::string(ToString(m_family)) +
            " player template found. Build the Game target, pass --player, or set NLS_PLAYER_TEMPLATE.";
    }
    return std::nullopt;
}

bool UnixDesktopPlatform::IsRuntimeTemplateFile(const std::filesystem::path& relativePath)
{
    const auto text = NormalizeSlashes(relativePath.generic_string());
    const auto name = relativePath.filename().string();
    const auto extension = relativePath.extension().string();

    if (name == "NullusBuild" || name == "NullusBuild.exe" || name == "Player.info" ||
        name == "NullusPlayer.json" || name == "imgui.ini" ||
        name.rfind("Editor", 0u) == 0u || name.rfind("Launcher", 0u) == 0u ||
        name.rfind("NullusBuild", 0u) == 0u)
    {
        return false;
    }
    if (text.rfind(".nullus-runtime-", 0u) == 0u)
        return false;
    if (text.rfind("Assets/Editor/", 0u) == 0u ||
        text.rfind("Assets/Templates/", 0u) == 0u ||
        text.rfind("Library/", 0u) == 0u ||
        text.rfind("Logs/", 0u) == 0u ||
        text.rfind("Tracy/", 0u) == 0u ||
        text.rfind("Tests/", 0u) == 0u ||
        text.rfind("build/", 0u) == 0u ||
        text.rfind("obj/", 0u) == 0u)
    {
        return false;
    }
    if (extension == ".pdb" || extension == ".log" || extension == ".lib" ||
        extension == ".exp" || extension == ".ilk" || extension == ".o" ||
        extension == ".a")
    {
        return false;
    }
    return true;
}

bool UnixDesktopPlatform::AssemblePlayer(
    const BuildContext& context,
    const PlayerTemplateInfo& player,
    BuildReport& report,
    std::string* error)
{
    bool copiedExecutable = false;
    bool bundleExecutable = false;
    for (const auto& relative : player.runtimeFiles)
    {
        const auto source = player.directory / relative;
        std::filesystem::path destinationRelative = relative;
        bool executable = false;
        if (IsBundleExecutable(relative))
        {
            const auto name = ExecutableName(context.profile);
            if (relative.generic_string().rfind("Game.app/", 0u) == 0u)
            {
                destinationRelative = std::filesystem::path("Game.app/Contents/MacOS") / name;
                bundleExecutable = true;
            }
            else
            {
                destinationRelative = name;
            }
            executable = true;
            copiedExecutable = true;
        }
        const auto destination = context.stagingRoot / destinationRelative;
        if (!CopyFileEnsuringDirectory(source, destination, error))
            return false;
        if (executable)
        {
            std::error_code permissionError;
            std::filesystem::permissions(
                destination,
                std::filesystem::perms::owner_exec |
                    std::filesystem::perms::group_exec |
                    std::filesystem::perms::others_exec,
                std::filesystem::perm_options::add,
                permissionError);
        }
    }
    if (!copiedExecutable)
    {
        if (error != nullptr)
            *error = "the Unix player template did not provide a Game executable";
        return false;
    }
    if (bundleExecutable)
    {
        if (!PatchMacBundleExecutable(
                context.stagingRoot / "Game.app",
                ExecutableName(context.profile),
                error))
        {
            return false;
        }
        report.AddDiagnostic(
            DiagnosticSeverity::Info,
            "build-player-bundle",
            "macOS bundle Info.plist was updated for the product executable.",
            ExecutableName(context.profile));
    }

    report.AddDiagnostic(
        DiagnosticSeverity::Info,
        "build-player-executable",
        "Unix player executable staged as '" + ExecutableName(context.profile) + "'.",
        player.executablePath.string());
    return true;
}

bool UnixDesktopPlatform::FinalizeStagingPlayer(
    const BuildContext& context,
    BuildReport& report,
    std::string* error)
{
    if (m_family == PlatformFamily::Linux)
    {
        size_t patched = 0u;
        std::error_code iterateError;
        for (const auto& entry : std::filesystem::directory_iterator(context.stagingRoot, iterateError))
        {
            if (iterateError)
                break;
            if (!entry.is_regular_file())
                continue;
            const auto name = entry.path().filename().string();
            if (name != ExecutableName(context.profile) && name.find(".so") == std::string::npos)
                continue;
            if (!PatchElfRunPath(entry.path(), error))
                return false;
            ++patched;
        }
        if (iterateError)
        {
            if (error != nullptr)
                *error = "cannot inspect the Unix player payload: " + iterateError.message();
            return false;
        }
        report.AddDiagnostic(
            DiagnosticSeverity::Info,
            "build-unix-rpath",
            "Linux runtime binaries were normalized to use $ORIGIN for relocatable libraries.",
            std::to_string(patched) + " file(s)");
    }

    const auto metadata = context.stagingRoot / "Player.metadata.txt";
    std::ofstream output(metadata, std::ios::binary | std::ios::trunc);
    if (output)
    {
        output << "productName=" << context.product.productName << "\n"
               << "companyName=" << context.product.companyName << "\n"
               << "version=" << context.product.version << "\n"
               << "buildNumber=" << context.product.buildNumber << "\n"
               << "executableName=" << context.product.executableName << "\n"
               << "targetId=" << BuildTargetId(context.profile.target) << "\n";
    }
    report.AddDiagnostic(
        DiagnosticSeverity::Info,
        "build-unix-metadata",
        "Unix product metadata was written.",
        metadata.string());
    return true;
}

bool UnixDesktopPlatform::SignStaging(
    const BuildContext& context,
    BuildReport& report,
    std::string* error)
{
    (void)error;
    if (!context.profile.signingProfile.empty())
    {
        report.AddDiagnostic(
            DiagnosticSeverity::Warning,
            "build-signing-unix",
            "Unix signing is not implemented; the payload stays unsigned.",
            context.profile.signingProfile);
    }
    report.signed_ = false;
    report.signingDescription = "unsigned (Unix signing not implemented)";
    return true;
}

bool UnixDesktopPlatform::PackageOutput(
    const BuildContext& context,
    const std::filesystem::path& stagingDirectory,
    BuildReport& report,
    std::filesystem::path& outputRoot,
    std::string* error)
{
    const auto packageName = MakeOutputDirectoryName(context.profile, context.profile.target);
    const auto packageDirectory = context.outputRoot / packageName;
    if (!EnsureDirectory(packageDirectory, error))
        return false;
    size_t copied = 0u;
    if (!CopyTreeFiltered(
            stagingDirectory,
            packageDirectory,
            [](const std::string&) { return true; },
            &copied,
            error))
    {
        return false;
    }
    report.packageDirectory = packageDirectory.string();
    outputRoot = context.outputRoot;

    const auto executable = ExecutableName(context.profile);
    const auto markExecutable = [](const std::filesystem::path& path)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
            return;
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_exec |
                std::filesystem::perms::group_exec |
                std::filesystem::perms::others_exec,
            std::filesystem::perm_options::add,
            error);
    };
    markExecutable(packageDirectory / executable);
    markExecutable(packageDirectory / "Runtime" / "dotnet" / "dotnet");
    if (m_family == PlatformFamily::MacOS)
        markExecutable(packageDirectory / "Game.app" / "Contents" / "MacOS" / executable);

    const auto executableSuffix = "/" + executable;
    const auto isExecutable = [&executableSuffix](const std::string& archivePath)
    {
        return archivePath.ends_with(executableSuffix) ||
            archivePath.ends_with("/Runtime/dotnet/dotnet") ||
            archivePath.find("/Contents/MacOS/") != std::string::npos;
    };

    if (context.profile.outputFormat == OutputFormat::Zip)
    {
        ZipArchiveWriter archive(context.outputRoot / (packageName + ".zip"));
        if (!archive.AddDirectory(packageName, packageDirectory, error, isExecutable) ||
            !archive.Write(error))
        {
            return false;
        }
    }
    else if (context.profile.outputFormat != OutputFormat::Directory)
    {
        if (error != nullptr)
        {
            *error = std::string("output format '") +
                ToString(context.profile.outputFormat) +
                "' is not implemented by the Unix desktop adapter (use zip or directory).";
        }
        return false;
    }
    return true;
}

bool UnixDesktopPlatform::ValidateOutput(
    const BuildContext& context,
    const std::filesystem::path& packageDirectory,
    BuildReport& report,
    std::string* error)
{
    if (!context.validateLaunch && !context.profile.validateLaunchOnBuild)
        return true;

    const auto executable = packageDirectory / ExecutableName(context.profile);
    auto stagedExecutable = executable;
    if (!std::filesystem::is_regular_file(stagedExecutable))
    {
        const auto bundled = packageDirectory / "Game.app" / "Contents" / "MacOS" /
            ExecutableName(context.profile);
        if (std::filesystem::is_regular_file(bundled))
            stagedExecutable = bundled;
    }
    if (!std::filesystem::is_regular_file(stagedExecutable))
    {
        if (error != nullptr)
            *error = "packaged executable missing for launch validation: " + executable.string();
        return false;
    }

    if (!HostMatches(m_family))
    {
        report.AddDiagnostic(
            DiagnosticSeverity::Warning,
            "build-launch-cross-host",
            "The package was assembled for another host; the packaged self-test was skipped.",
            stagedExecutable.string());
        return true;
    }

    std::ostringstream command;
    command << '"' << stagedExecutable.string() << "\" --package-self-test";
    const auto result = std::system(command.str().c_str());
    if (result != 0)
    {
        if (error != nullptr)
            *error = "packaged self-test failed for '" + stagedExecutable.string() +
                "' (exit code " + std::to_string(result) + ")";
        return false;
    }
    report.AddDiagnostic(
        DiagnosticSeverity::Info,
        "build-launch-validated",
        "The packaged Unix player completed its content self-test.",
        stagedExecutable.string());
    return true;
}

void RegisterLinuxDesktopPlatform()
{
    BuildPlatformRegistry::Get().Register(
        std::make_shared<UnixDesktopPlatform>(PlatformFamily::Linux));
}

void RegisterMacOSDesktopPlatform()
{
    BuildPlatformRegistry::Get().Register(
        std::make_shared<UnixDesktopPlatform>(PlatformFamily::MacOS));
}
} // namespace NLS::Build
