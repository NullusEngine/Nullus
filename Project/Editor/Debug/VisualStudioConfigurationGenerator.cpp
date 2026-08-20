#include "VisualStudioConfigurationGenerator.h"
#include "ProjectDebugWorkspace.h"
#include <Platform/Process/Process.h>

#include <Json/json.hpp>

#include <fstream>
#include <system_error>
#include <utility>

namespace NLS::Editor::Debug
{
namespace
{
using Json = nlohmann::json;

bool WriteText(const std::filesystem::path& path, const std::string& text, std::string& error)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        error = "Unable to write " + path.string();
        return false;
    }
    output << text;
    if (!output)
    {
        error = "Unable to flush " + path.string();
        return false;
    }
    return true;
}

std::string XmlEscape(std::string value)
{
    const auto replace = [&value](const char* from, const char* to)
    {
        std::string::size_type offset = 0;
        while ((offset = value.find(from, offset)) != std::string::npos)
        {
            value.replace(offset, std::char_traits<char>::length(from), to);
            offset += std::char_traits<char>::length(to);
        }
    };
    replace("&", "&amp;");
    replace("<", "&lt;");
    replace(">", "&gt;");
    replace("\"", "&quot;");
    return value;
}

}

VisualStudioConfigurationResult GenerateVisualStudioConfiguration(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& editorExecutable)
{
    const auto editor = editorExecutable.empty()
        ? NLS::Platform::Process::GetCurrentExecutablePath()
        : editorExecutable;
    const auto broker = editor.parent_path() /
#ifdef _WIN32
        "NullusDebugBroker.exe";
#else
        "NullusDebugBroker";
#endif
    const auto workspace = GenerateProjectDebugWorkspace(projectRoot, editor, broker);
    if (!workspace.success)
    {
        VisualStudioConfigurationResult result;
        result.errorMessage = workspace.errorMessage;
        return result;
    }
    return GenerateVisualStudioConfiguration(workspace.manifest);
}

VisualStudioConfigurationResult GenerateVisualStudioConfiguration(
    const ProjectDebugManifest& manifest)
{
    VisualStudioConfigurationResult result;
    if (manifest.projectRoot.empty() || manifest.visualStudioProject.empty() ||
        manifest.visualStudioSolution.empty() || manifest.brokerExecutable.empty())
    {
        result.errorMessage = "The Nullus project debug manifest is incomplete.";
        return result;
    }

    const auto visualStudioRoot = manifest.visualStudioProject.parent_path();
    const auto metadataRoot = visualStudioRoot / ".vs";
    const auto propertiesRoot = visualStudioRoot / "Properties";
    std::error_code error;
    std::filesystem::create_directories(metadataRoot, error);
    std::filesystem::create_directories(propertiesRoot, error);
    if (error)
    {
        result.errorMessage = "Unable to create the project Visual Studio metadata directory: " + error.message();
        return result;
    }

    // The provider is deliberately described in a project-local file. The
    // bundled VSIX reads this file and invokes the same Broker as VS Code;
    // no global VS settings or shared .csproj.user file are changed.
    const auto launchProfile = propertiesRoot / "launchSettings.json";
    const auto manifestPath = (manifest.workspaceRoot / "Nullus.Debug.json").string();
    const auto makeProfile = [&](const char* mode, const bool play, const char* configuration)
    {
        return Json{
            {"commandName", "NullusEditor"},
            {"nullusManifest", manifestPath},
            {"nullusBroker", manifest.brokerExecutable.string()},
            {"nullusMode", mode},
            {"nullusConfiguration", configuration},
            {"nullusPlayAfterAttach", play}};
    };
    Json profiles = Json::object();
    profiles["Nullus: C# Scripts"] = makeProfile("managed", false, "Debug");
    profiles["Nullus: C# Attach and Play"] = makeProfile("managed", true, "Debug");
    if (manifest.mixedDebugAvailable)
    {
        profiles["Nullus: Editor + C# Mixed"] = makeProfile("mixed", false, "Debug");
        profiles["Nullus: Mixed Attach and Play"] = makeProfile("mixed", true, "Debug");
    }
    const Json launch = {
        {"profiles", profiles},
        {"$schema", "http://json.schemastore.org/launchsettings.json"}
    };
    if (!WriteText(launchProfile, launch.dump(4) + "\n", result.errorMessage))
        return result;

    // Keep the project-local metadata file for older bundled VSIX builds. It
    // points to the same standard launch profile and is never written to the
    // shared repository solution.
    const auto legacyProfile = visualStudioRoot / "Nullus.Debug.vs.json";
    const Json legacy = {
        {"version", "2.0"},
        {"provider", "NullusEditor"},
        {"projectId", manifest.projectId},
        {"manifest", manifestPath},
        {"broker", manifest.brokerExecutable.string()},
        {"editor", manifest.editorExecutable.string()},
        {"launchSettings", launchProfile.string()}
    };
    if (!WriteText(legacyProfile, legacy.dump(4) + "\n", result.errorMessage))
        return result;

    auto projectUserPath = manifest.visualStudioProject;
    projectUserPath.replace_extension(".csproj.user");
    const std::string userContent =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<Project ToolsVersion=\"Current\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
        "  <PropertyGroup>\n"
        "    <NullusDebugProvider>NullusEditor</NullusDebugProvider>\n"
        "    <NullusDebugManifest>" + XmlEscape((manifest.workspaceRoot / "Nullus.Debug.json").string()) + "</NullusDebugManifest>\n"
        "    <NullusDebugLaunchProfile>" + XmlEscape(launchProfile.string()) + "</NullusDebugLaunchProfile>\n"
         "    <NullusDebugConfiguration>C# Scripts</NullusDebugConfiguration>\n"
        "    <NullusDebugEngine>CoreCLR</NullusDebugEngine>\n"
        "  </PropertyGroup>\n"
        "</Project>\n";
    if (!WriteText(projectUserPath, userContent, result.errorMessage))
        return result;

    result.success = true;
    result.projectWorkspacePath = visualStudioRoot;
    result.launchProfilePath = launchProfile;
    result.projectUserPath = projectUserPath;
    return result;
}
}
