#include "Windows/WindowsDesktopPlatform.h"

#include "ArchiveWriter.h"
#include "BuildFileUtils.h"
#include "BuildPipeline.h"
#include "BuildReport.h"

#include "Engine/Assets/PlayerDescriptor.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace NLS::Build
{
void RegisterWindowsDesktopPlatform()
{
    BuildPlatformRegistry::Get().Register(std::make_shared<WindowsDesktopPlatform>());
}
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

std::string MakeOutputDirectoryName(const BuildProfile& profile, const BuildTarget& target)
{
    std::string name = SanitizeName(profile.productName.empty() ? profile.executableName : profile.productName);
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

std::string GetEnvironmentValue(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string {};
}

std::string ReadRuntimeManifestForTemplate(
    const std::filesystem::path& templateDirectory,
    const std::string& configuration)
{
    if (!configuration.empty())
    {
        const auto named = templateDirectory / (".nullus-runtime-" + configuration + ".manifest");
        if (const auto text = ReadTextFileOrEmpty(named); !text.empty())
            return text;
    }
    // Fall back to whichever deployment manifest the template carries.
    for (const auto& file : EnumerateFiles(templateDirectory, false))
    {
        const auto name = file.filename().string();
        if (name.rfind(".nullus-runtime-", 0u) == 0u && file.extension() == ".manifest")
        {
            if (const auto text = ReadTextFileOrEmpty(file); !text.empty())
                return text;
        }
    }
    return {};
}

std::vector<std::string> ParseManifestLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty())
            lines.push_back(line);
    }
    return lines;
}

std::string FindSigntool()
{
    if (const auto configured = GetEnvironmentValue("NLS_SIGNTOOL_PATH"); !configured.empty())
        return configured;
    // Probe the common Windows SDK locations newest-first.
    for (const char* root : {"C:/Program Files (x86)/Windows Kits/10/bin", "C:/Program Files/Windows Kits/10/bin"})
    {
        std::error_code error;
        const std::filesystem::path rootPath(root);
        if (!std::filesystem::is_directory(rootPath, error))
            continue;
        std::vector<std::filesystem::path> versions;
        for (const auto& entry : std::filesystem::directory_iterator(rootPath, error))
        {
            if (entry.is_directory())
                versions.push_back(entry.path());
        }
        std::sort(versions.begin(), versions.end(), std::greater<>());
        for (const auto& version : versions)
        {
            for (const char* architecture : {"x64", "x86"})
            {
                const auto candidate = version / architecture / "signtool.exe";
                if (std::filesystem::is_regular_file(candidate, error))
                    return candidate.string();
            }
        }
    }
    return {};
}
} // namespace

std::string WindowsDesktopPlatform::GetAdapterId() const
{
    return "windows-x64-desktop";
}

PlatformCapability WindowsDesktopPlatform::GetCapability(const BuildTarget& target) const
{
    PlatformCapability capability;
    capability.platform = PlatformFamily::Windows;
    capability.architecture = Architecture::X64;
    capability.graphicsBackend = target.graphicsBackend;
    capability.id = std::string("windows-x64-") + ToString(target.graphicsBackend);

    if (target.platform != PlatformFamily::Windows || target.architecture != Architecture::X64)
    {
        capability.reason = "The desktop adapter only builds Windows x64 players.";
        return capability;
    }
    if (target.graphicsBackend != GraphicsBackendKind::DX12 &&
        target.graphicsBackend != GraphicsBackendKind::Vulkan)
    {
        capability.reason = "Windows desktop supports DX12 and Vulkan.";
        return capability;
    }

    capability.adapterAvailable = true;
#if defined(_WIN32)
    capability.toolchainAvailable = true;
#else
    capability.toolchainAvailable = false;
    capability.reason = "Windows packaging runs on a Windows host.";
#endif
    return capability;
}

bool WindowsDesktopPlatform::CheckToolchain(
    const BuildContext& context,
    BuildReport& report,
    std::string* error)
{
    (void)context;
    (void)report;
#if !defined(_WIN32)
    if (error != nullptr)
        *error = "Windows desktop packaging requires a Windows build host.";
    return false;
#else
    return true;
#endif
}

std::string WindowsDesktopPlatform::ResolveWindowsExecutableName(const BuildProfile& profile)
{
    const auto base = profile.executableName.empty()
        ? (profile.productName.empty() ? std::string("NullusGame") : profile.productName)
        : profile.executableName;
    return SanitizeName(base) + ".exe";
}

bool WindowsDesktopPlatform::IsRuntimeTemplateFile(const std::filesystem::path& relativePath)
{
    const auto text = ToForwardSlashes(relativePath.generic_string());
    const auto name = relativePath.filename().string();
    const auto extension = relativePath.extension().string();

    if (name == "Editor.exe" || name == "Launcher.exe")
        return false;
    // Build tooling that happens to sit next to the editor in the shared App
    // directory must never be copied into a player.
    if (name == "NLS_Build.dll")
        return false;
    if (name == "NLS_Cook.dll")
        return false;
    if (name == "NullusPlayer.json" || name == "Player.info")
        return false;
    if (name == "imgui.ini" || name == "vc140.pdb")
        return false;
    if (extension == ".log" || extension == ".pdb" || extension == ".lib" || extension == ".exp")
        return false;
    if (text.rfind("Assets/Editor/", 0u) == 0u ||
        text.rfind("Assets/Templates/", 0u) == 0u)
    {
        return false;
    }
    if (text.rfind("build/", 0u) == 0u ||
        text.rfind("Tracy/", 0u) == 0u ||
        text.rfind("Tests/", 0u) == 0u ||
        text.rfind(".nullus-runtime-", 0u) == 0u)
    {
        return false;
    }
    if (name.rfind(".nullus-runtime-", 0u) == 0u)
        return false;
    return true;
}

std::optional<PlayerTemplateInfo> WindowsDesktopPlatform::ResolvePlayerTemplate(
    const BuildContext& context,
    BuildReport& report,
    std::string* error)
{
    std::vector<std::filesystem::path> candidates;
    if (!context.playerTemplatePath.empty())
        candidates.push_back(context.playerTemplatePath);
    if (!context.profile.playerTemplatePath.empty())
    {
        const std::filesystem::path configured(context.profile.playerTemplatePath);
        candidates.push_back(configured.is_absolute() ? configured : context.projectRoot / configured);
    }
    if (const auto environment = GetEnvironmentValue("NLS_PLAYER_TEMPLATE"); !environment.empty())
        candidates.push_back(environment);
    if (!context.editorExecutablePath.empty())
        candidates.push_back(context.editorExecutablePath.parent_path());

    for (auto& candidate : candidates)
    {
        std::error_code candidateError;
        if (!std::filesystem::is_directory(candidate, candidateError))
            continue;

        const auto executablePath = candidate / "Game.exe";
        if (!std::filesystem::is_regular_file(executablePath, candidateError))
            continue;

        PlayerTemplateInfo info;
        info.directory = candidate;
        info.executablePath = executablePath;
        info.executableName = ResolveWindowsExecutableName(context.profile);
        info.sourceBuilt = !context.profile.customNativeModules.empty();

        if (const auto descriptor = NLS::Engine::Assets::PlayerDescriptor::Load(candidate / "Player.info", nullptr);
            descriptor.has_value())
        {
            info.engineVersion = descriptor->engineVersion;
            info.playerId = descriptor->playerId;
            info.targetId = descriptor->targetId;
        }

        const auto configuration = BuildModeConfiguration(context.profile.mode);
        const auto manifestLines = ParseManifestLines(ReadRuntimeManifestForTemplate(candidate, configuration));
        std::set<std::string> runtimeFileNames(manifestLines.begin(), manifestLines.end());
        runtimeFileNames.insert("Game.exe");

        // The engine asset tree, managed payload, and dotnet runtime travel
        // with the player; the deployment manifest covers only first-party
        // binary dependencies.
        for (const auto& file : EnumerateFiles(candidate, true))
        {
            std::error_code relativeError;
            const auto relative = std::filesystem::relative(file, candidate, relativeError);
            if (relativeError)
                continue;
            if (!IsRuntimeTemplateFile(relative))
                continue;
            const auto relativeText = ToForwardSlashes(relative.generic_string());
            if (manifestLines.empty() || runtimeFileNames.count(relativeText) != 0u ||
                relativeText.rfind("Assets/", 0u) == 0u ||
                relativeText.rfind("Managed/", 0u) == 0u ||
                relativeText.rfind("dotnet/", 0u) == 0u ||
                relativeText.rfind("Tools/Dotnet/", 0u) == 0u ||
                relativeText.rfind("Scripts/", 0u) == 0u)
            {
                info.runtimeFiles.push_back(relative);
            }
            if (relativeText.rfind("Scripts/", 0u) == 0u || relativeText.rfind("Managed/", 0u) == 0u)
                info.hasManagedAssemblies = true;
            if (relativeText.rfind("Assets/Scripts/", 0u) == 0u)
                info.hasLuaScripts = true;
        }
        for (const auto& file : EnumerateFiles(candidate, true))
        {
            if (file.extension() != ".pdb")
                continue;
            // The shared App directory also contains editor/launcher symbols;
            // never deliver those with a player.
            const auto symbolName = file.filename().string();
            if (symbolName == "Editor.pdb" || symbolName == "Launcher.pdb" ||
                symbolName == "NullusBuild.pdb")
            {
                continue;
            }
            info.symbolFiles.push_back(file);
        }

        if (info.playerId.empty())
        {
            info.playerId = ComputeFileSha256(executablePath).substr(0u, 16u);
            report.AddDiagnostic(
                DiagnosticSeverity::Warning,
                "build-player-template-descriptor",
                "The player template has no Player.info; the player identity is derived from the executable hash.",
                candidate.string());
        }
        if (info.engineVersion.empty())
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Warning,
                "build-player-template-version",
                "The player template does not record an engine version; compatibility relies on the target descriptor.",
                candidate.string());
        }
        if (info.runtimeFiles.empty())
        {
            if (error != nullptr)
                *error = "player template '" + candidate.string() + "' contains no runtime files";
            return std::nullopt;
        }
        return info;
    }

    if (error != nullptr)
    {
        *error = "no player template found. Build the engine Game target, or pass --player <directory> with a "
                 "directory containing Game.exe and its runtime dependencies.";
    }
    return std::nullopt;
}

bool WindowsDesktopPlatform::AssemblePlayer(
    const BuildContext& context,
    const PlayerTemplateInfo& player,
    BuildReport& report,
    std::string* error)
{
    std::set<std::string> copied;
    for (const auto& relative : player.runtimeFiles)
    {
        const auto source = player.directory / relative;
        std::error_code relativeError;
        if (!std::filesystem::is_regular_file(source, relativeError))
            continue;

        auto relativeText = ToForwardSlashes(relative.generic_string());
        std::filesystem::path destinationRelative(relativeText);
        if (relativeText == "Game.exe")
            destinationRelative = player.executableName;
        const auto destination = context.stagingRoot / destinationRelative;

        if (!CopyFileEnsuringDirectory(source, destination, error))
            return false;
        copied.insert(relativeText);

        if (relativeText == "Game.exe")
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Info,
                "build-player-executable",
                "Player executable staged as '" + player.executableName + "'.",
                source.string());
        }
    }

    if (copied.find("Game.exe") == copied.end())
    {
        if (error != nullptr)
            *error = "the player template did not provide Game.exe";
        return false;
    }

    // Keep the icon next to the executable while PE resource editing is a
    // template-builder responsibility.
    if (!context.profile.iconPath.empty())
    {
        const std::filesystem::path icon(context.profile.iconPath);
        const auto source = icon.is_absolute() ? icon : context.projectRoot / icon;
        std::error_code iconError;
        if (std::filesystem::is_regular_file(source, iconError))
        {
            if (!CopyFileEnsuringDirectory(source, context.stagingRoot / source.filename(), error))
                return false;
        }
        else
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Warning,
                "build-icon-missing",
                "The profile icon file was not found.",
                source.string());
        }
    }
    return true;
}

bool WindowsDesktopPlatform::FinalizeStagingPlayer(
    const BuildContext& context,
    BuildReport& report,
    std::string* error)
{
    (void)error;
    if (!context.profile.customNativeModules.empty())
    {
        report.AddDiagnostic(
            DiagnosticSeverity::Warning,
            "build-native-modules-template",
            "Custom native modules are taken from the player template; verify the template was source-built from this project.",
            context.profile.playerTemplatePath);
    }

    // PE resource editing (icon, version info) is delegated to the player
    // template builder; record the product metadata that the template embeds.
    const auto metadataPath = context.stagingRoot / "Player.metadata.txt";
    std::ofstream metadata(metadataPath, std::ios::binary | std::ios::trunc);
    if (metadata)
    {
        metadata << "productName=" << context.product.productName << "\n";
        metadata << "companyName=" << context.product.companyName << "\n";
        metadata << "version=" << context.product.version << "\n";
        metadata << "buildNumber=" << context.product.buildNumber << "\n";
        metadata << "executableName=" << context.product.executableName << "\n";
    }
    return true;
}

bool WindowsDesktopPlatform::SignStaging(
    const BuildContext& context,
    BuildReport& report,
    std::string* error)
{
    report.signed_ = false;
    if (context.profile.signingProfile.empty() || !context.sign)
    {
        report.signingDescription = "unsigned (no signing credential reference)";
        report.AddDiagnostic(
            DiagnosticSeverity::Info,
            "build-signing-skipped",
            "No Authenticode credential reference is configured; the output is unsigned.",
            context.profile.id);
        return true;
    }

    std::string certificate = GetEnvironmentValue("NLS_SIGNING_CERT_SHA1");
    if (certificate.empty())
    {
        auto reference = context.profile.signingProfile;
        if (reference.rfind("env:", 0u) == 0u)
            reference = reference.substr(4u);
        certificate = GetEnvironmentValue(reference.c_str());
    }
    if (certificate.empty())
    {
        if (error != nullptr)
        {
            *error = "signing profile '" + context.profile.signingProfile +
                "' could not be resolved from CI secrets or the system credential store.";
        }
        return false;
    }

    const auto signtool = FindSigntool();
    if (signtool.empty())
    {
        if (error != nullptr)
            *error = "signtool.exe was not found; install the Windows SDK or set NLS_SIGNTOOL_PATH.";
        return false;
    }

    size_t signedFiles = 0u;
    for (const auto& file : EnumerateFiles(context.stagingRoot, false))
    {
        const auto extension = file.extension().string();
        if (extension != ".exe" && extension != ".dll")
            continue;
        std::ostringstream command;
        command << '"' << signtool << "\" sign /sha1 " << certificate << " /fd SHA256 /td SHA256 /tr "
                << "http://timestamp.digicert.com" << " \"" << file.string() << "\"";
        const auto result = std::system(command.str().c_str());
        if (result != 0)
        {
            if (error != nullptr)
                *error = "signtool failed for '" + file.string() + "'";
            return false;
        }
        ++signedFiles;
    }

    report.signed_ = signedFiles > 0u;
    report.signingDescription = report.signed_
        ? ("Authenticode signed " + std::to_string(signedFiles) + " file(s)")
        : "unsigned (no signable binaries staged)";
    return true;
}

bool WindowsDesktopPlatform::PackageOutput(
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

    size_t copiedCount = 0u;
    if (!CopyTreeFiltered(
            stagingDirectory,
            packageDirectory,
            [](const std::string&) { return true; },
            &copiedCount,
            error))
    {
        return false;
    }

    report.packageDirectory = packageDirectory.string();
    outputRoot = context.outputRoot;

    if (context.profile.outputFormat == OutputFormat::Zip)
    {
        ZipArchiveWriter archive(context.outputRoot / (packageName + ".zip"));
        if (!archive.AddDirectory(packageName, packageDirectory, error))
            return false;
        if (!archive.Write(error))
            return false;
    }
    else if (context.profile.outputFormat != OutputFormat::Directory)
    {
        if (error != nullptr)
        {
            *error = std::string("output format '") + ToString(context.profile.outputFormat) +
                "' is not implemented by the Windows desktop adapter.";
        }
        return false;
    }
    return true;
}
bool WindowsDesktopPlatform::ValidateOutput(
    const BuildContext& context,
    const std::filesystem::path& packageDirectory,
    BuildReport& report,
    std::string* error)
{
    if (!context.validateLaunch && !context.profile.validateLaunchOnBuild)
        return true;

    const auto executable = packageDirectory / (context.product.executableName + ".exe");
    if (!std::filesystem::is_regular_file(executable))
    {
        if (error != nullptr)
            *error = "packaged executable missing for launch validation: " + executable.string();
        return false;
    }

    // The player's `--package-self-test` mode mounts the shipped packages,
    // loads the runtime manifest, checks the start scene, and exits without
    // initializing a graphics device. This makes runtime isolation testable in
    // CI and on machines without a display.
    std::ostringstream command;
    command << '"' << executable.string() << "\" --package-self-test";
    const auto result = std::system(command.str().c_str());
    if (result != 0)
    {
        if (error != nullptr)
            *error = "packaged self-test failed for '" + executable.string() +
                "' (exit code " + std::to_string(result) + ")";
        return false;
    }
    report.AddDiagnostic(
        DiagnosticSeverity::Info,
        "build-launch-validated",
        "The packaged player completed its content self-test.",
        executable.string());
    return true;
}
} // namespace NLS::Build
