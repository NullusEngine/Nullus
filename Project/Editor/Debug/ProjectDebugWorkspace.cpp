#include "ProjectDebugWorkspace.h"

#include "VSCodeConfigurationGenerator.h"
#include "VisualStudioConfigurationGenerator.h"

#include <Json/json.hpp>
#include <Assets/ArtifactManifest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace NLS::Editor::Debug
{
namespace
{
using Json = nlohmann::json;

std::filesystem::path CanonicalOrNormalized(std::filesystem::path path)
{
    if (path.empty())
        return {};
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : canonical;
}

std::filesystem::path FindProjectFile(const std::filesystem::path& root)
{
    std::error_code error;
    std::vector<std::filesystem::path> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(root, error))
    {
        if (error)
            break;
        if (entry.is_regular_file(error) && entry.path().extension() == ".nullus")
            candidates.push_back(entry.path());
        error.clear();
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.empty() ? std::filesystem::path{} : CanonicalOrNormalized(candidates.front());
}

std::filesystem::path ResolveProjectFile(const std::filesystem::path& projectPath)
{
    if (projectPath.empty())
        return {};
    std::error_code error;
    const auto normalized = CanonicalOrNormalized(projectPath);
    if (std::filesystem::is_regular_file(normalized, error) && normalized.extension() == ".nullus")
        return normalized;
    if (!std::filesystem::is_directory(normalized, error))
        return {};
    return FindProjectFile(normalized);
}

std::filesystem::path FindRepositoryRoot(const std::filesystem::path& root)
{
    std::error_code error;
    auto current = CanonicalOrNormalized(root);
    for (;;)
    {
        if (std::filesystem::is_regular_file(current / "Managed" / "Nullus.GameScripts" / "Nullus.GameScripts.csproj", error))
            return current;
        error.clear();
        const auto parent = current.parent_path();
        if (parent.empty() || parent == current)
            break;
        current = parent;
    }
    return {};
}

std::filesystem::path FindEngineWorkspaceDescriptor(const std::filesystem::path& repositoryRoot)
{
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(repositoryRoot / "build" / "Nullus.EngineWorkspace.json");
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(repositoryRoot, error))
    {
        if (error)
            break;
        if (!entry.is_directory(error))
        {
            error.clear();
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.rfind("build", 0u) == 0u)
            candidates.push_back(entry.path() / "Nullus.EngineWorkspace.json");
        error.clear();
    }
    for (const auto& candidate : candidates)
    {
        if (std::filesystem::is_regular_file(candidate, error))
            return CanonicalOrNormalized(candidate);
        error.clear();
    }
    return {};
}

bool HasNativeSource(const std::filesystem::path& root)
{
    if (!std::filesystem::is_directory(root))
        return false;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error))
    {
        if (error)
            break;
        if (!entry.is_regular_file(error))
        {
            error.clear();
            continue;
        }
        const auto extension = entry.path().extension().string();
        if (extension == ".cpp" || extension == ".c" || extension == ".cc" ||
            extension == ".h" || extension == ".hpp" || extension == ".inl" || extension == ".mm")
            return true;
        error.clear();
    }
    return false;
}

std::string ReadProjectGuid(const std::filesystem::path& projectPath)
{
    std::ifstream input(projectPath, std::ios::binary);
    if (!input)
        return {};
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto begin = text.find("<ProjectGuid>");
    if (begin == std::string::npos)
        return {};
    const auto valueBegin = begin + std::string("<ProjectGuid>").size();
    const auto end = text.find("</ProjectGuid>", valueBegin);
    if (end == std::string::npos)
        return {};
    return text.substr(valueBegin, end - valueBegin);
}

std::string FileIdentity(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        return {};
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};
    const auto timestamp = std::filesystem::last_write_time(path, error);
    if (error)
        return std::to_string(size);
    const auto timestampCount = std::chrono::duration_cast<std::chrono::nanoseconds>(
        timestamp.time_since_epoch()).count();
    return std::to_string(size) + ":" + std::to_string(timestampCount);
}

std::string NormalizeProjectIdentity(std::filesystem::path path)
{
    path = CanonicalOrNormalized(std::move(path));
    auto value = path.generic_string();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
#endif
    return value;
}

uint64_t ComputeWorkspaceRevision(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& repositoryRoot)
{
    uint64_t hash = 1469598103934665603ull;
    const auto add = [&hash](std::string_view text)
    {
        for (const auto character : text)
        {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1099511628211ull;
        }
    };
    std::vector<std::filesystem::path> files;
    std::error_code error;
    const auto assets = projectRoot / "Assets";
    if (std::filesystem::exists(assets, error))
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(assets, error))
        {
            if (error)
                break;
            if (!entry.is_regular_file(error))
            {
                error.clear();
                continue;
            }
            const auto extension = entry.path().extension().string();
            if (extension == ".cs" || extension == ".lua")
                files.push_back(entry.path());
            error.clear();
        }
    }
    const auto schema = repositoryRoot / "Library" / "ScriptApi" / "ScriptApi.json";
    if (std::filesystem::is_regular_file(schema, error))
        files.push_back(schema);
    std::sort(files.begin(), files.end());
    for (const auto& file : files)
    {
        add(file.lexically_relative(projectRoot).generic_string());
        std::ifstream input(file, std::ios::binary);
        if (input)
            add(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
    }
    return hash;
}

std::filesystem::path FindBundledVisualStudioExtension(const std::filesystem::path& repositoryRoot)
{
    const auto source = repositoryRoot / "Tools" / "Debug" / "VisualStudioExtension" / "Nullus.ScriptDebugger.vsix";
    if (std::filesystem::is_regular_file(source))
        return CanonicalOrNormalized(source);
    const auto release = repositoryRoot / "Tools" / "Debug" / "VisualStudioExtension" /
        "bin" / "Release" / "net48" / "Nullus.ScriptDebugger.vsix";
    return std::filesystem::is_regular_file(release) ? CanonicalOrNormalized(release) : std::filesystem::path{};
}

std::string MakeGuid(std::string_view value)
{
    // The workspace GUID only needs to be deterministic.  FNV-1a pairs avoid
    // adding another crypto dependency to the editor-side generator.
    uint64_t first = 1469598103934665603ull;
    uint64_t second = 1099511628211ull;
    for (const unsigned char character : value)
    {
        first ^= character;
        first *= 1099511628211ull;
        second ^= static_cast<uint64_t>(character) + 0x9e3779b97f4a7c15ull;
        second *= 14029467366897019727ull;
    }
    std::ostringstream output;
    output << '{' << std::hex << std::setfill('0')
        << std::setw(8) << static_cast<uint32_t>(first >> 32)
        << '-' << std::setw(4) << static_cast<uint16_t>(first >> 16)
        << '-' << std::setw(4) << static_cast<uint16_t>(first)
        << '-' << std::setw(4) << static_cast<uint16_t>(second >> 48)
        << '-' << std::setw(12) << (second & 0xffffffffffffull) << '}';
    return output.str();
}

bool WriteTextIfChanged(const std::filesystem::path& path, const std::string& text, std::string& error)
{
    std::ifstream existing(path, std::ios::binary);
    const std::string oldText{std::istreambuf_iterator<char>(existing), std::istreambuf_iterator<char>()};
    if (existing && oldText == text)
        return true;

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

bool WriteManifest(const ProjectDebugManifest& manifest, std::string& error)
{
    Json projects = Json::array();
    for (const auto& project : manifest.engineProjects)
    {
        projects.push_back({
            {"name", project.name},
            {"path", project.path},
            {"sourceRoot", project.sourceRoot},
            {"kind", project.kind}});
    }
    Json value = {
        {"schemaVersion", manifest.schemaVersion},
        {"protocolVersion", Json{{"major", manifest.protocolMajor}, {"minor", manifest.protocolMinor}}},
        {"protocolMajor", manifest.protocolMajor},
        {"protocolMinor", manifest.protocolMinor},
        {"workspaceRevision", manifest.workspaceRevision},
        {"engineBuildId", manifest.engineBuildId},
        {"brokerVersion", manifest.brokerVersion},
        {"requiredVsixVersion", manifest.requiredVsixVersion},
        {"projectId", manifest.projectId},
        {"projectRoot", manifest.projectRoot.string()},
        {"projectFile", manifest.projectFile.string()},
        {"workspaceRoot", manifest.workspaceRoot.string()},
        {"visualStudioSolution", manifest.visualStudioSolution.string()},
        {"visualStudioScriptsFilter", manifest.visualStudioScriptsFilter.string()},
        {"visualStudioEngineFilter", manifest.visualStudioEngineFilter.string()},
        {"visualStudioProject", manifest.visualStudioProject.string()},
        {"visualStudioCodeWorkspace", manifest.visualStudioCodeWorkspace.string()},
        {"visualStudioExtension", manifest.visualStudioExtension.string()},
        {"editorExecutable", manifest.editorExecutable.string()},
        {"brokerExecutable", manifest.brokerExecutable.string()},
        {"engineWorkspaceDescriptor", manifest.engineWorkspaceDescriptor.string()},
        {"engineSourceRoot", manifest.engineSourceRoot.string()},
        {"engineBuildRoot", manifest.engineBuildRoot.string()},
        {"engineSolution", manifest.engineSolution.string()},
        {"cmakeExecutable", manifest.cmakeExecutable.string()},
        {"nativeTarget", manifest.nativeTarget},
        {"nativeConfiguration", manifest.nativeConfiguration},
        {"nativeBuildId", manifest.nativeBuildId},
        {"nativePdbSignature", manifest.nativePdbSignature},
        {"engineSourceAvailable", manifest.engineSourceAvailable},
        {"nativeSymbolsAvailable", manifest.nativeSymbolsAvailable},
        {"mixedDebugAvailable", manifest.mixedDebugAvailable},
        {"engineProjects", projects},
    };
    return WriteTextIfChanged(
        manifest.workspaceRoot / "Nullus.Debug.json",
        value.dump(4) + "\n",
        error);
}

bool WriteProjectFile(const ProjectDebugManifest& manifest, const std::filesystem::path& repositoryRoot, std::string& error)
{
    // Keep the managed editing project independent from the engine projects.
    // Native projects are added only to the generated solution and never as
    // CPS ProjectReference entries, so Visual Studio does not load the shared
    // managed build graph into GameScripts.
    auto managedAssembly = repositoryRoot / "build" / "Managed" / "Debug" / "Nullus.Managed.dll";
    if (!std::filesystem::is_regular_file(managedAssembly))
        managedAssembly = repositoryRoot / "Managed" / "Nullus.Managed" / "bin" / "Debug" / "net8.0" / "Nullus.Managed.dll";
    auto analysisAssembly = repositoryRoot / "build" / "Managed" / "Debug" / "Nullus.ScriptAnalysis.dll";
    if (!std::filesystem::is_regular_file(analysisAssembly))
        analysisAssembly = repositoryRoot / "Managed" / "Nullus.ScriptAnalysis" / "bin" / "Debug" / "net8.0" / "Nullus.ScriptAnalysis.dll";

    std::string project;
    project += "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
    project += "  <PropertyGroup>\n";
    project += "    <TargetFramework>net8.0</TargetFramework>\n";
    project += "    <AssemblyName>GameScripts</AssemblyName>\n";
    project += "    <RootNamespace>Nullus.GameScripts</RootNamespace>\n";
    project += "    <Nullable>enable</Nullable>\n";
    project += "    <ImplicitUsings>enable</ImplicitUsings>\n";
    project += "    <LangVersion>latest</LangVersion>\n";
    project += "    <DebugType>portable</DebugType>\n";
    project += "    <DebugSymbols>true</DebugSymbols>\n";
    project += "    <Deterministic>true</Deterministic>\n";
    project += "    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>\n";
    project += "    <NullusProjectAssets>" + XmlEscape((manifest.projectRoot / "Assets").string()) + "</NullusProjectAssets>\n";
    // The generated IDE project is an editing/debugging view.  The Editor's
    // ManagedScriptBuildService owns the real generator build and supplies the
    // analyzer to the runtime project.  Loading a second analyzer here makes
    // Visual Studio compile the same generated manifest twice and can load a
    // generator built by a different Roslyn version.
    project += "  </PropertyGroup>\n";
    project += "  <ItemGroup>\n";
    project += "    <Compile Include=\"" + XmlEscape((manifest.projectRoot / "Assets" / "**" / "*.cs").string()) + "\" Link=\"Assets\\%(RecursiveDir)%(Filename)%(Extension)\" Condition=\"Exists('" + XmlEscape((manifest.projectRoot / "Assets").string()) + "')\" />\n";
    project += "    <None Include=\"" + XmlEscape((manifest.projectRoot / "Assets" / "**" / "*.lua").string()) + "\" Link=\"Assets\\%(RecursiveDir)%(Filename)%(Extension)\" Condition=\"Exists('" + XmlEscape((manifest.projectRoot / "Assets").string()) + "')\" />\n";
    project += "    <Reference Include=\"Nullus.Managed\">\n";
    project += "      <HintPath>" + XmlEscape(managedAssembly.string()) + "</HintPath>\n";
    project += "      <Private>false</Private>\n";
    project += "    </Reference>\n";
    if (std::filesystem::is_regular_file(analysisAssembly))
    {
        project += "    <Analyzer Include=\"" + XmlEscape(analysisAssembly.string()) + "\" />\n";
    }
    project += "    <AdditionalFiles Include=\"" + XmlEscape((repositoryRoot / "Library" / "ScriptApi" / "ScriptApi.json").string()) + "\" Condition=\"Exists('" + XmlEscape((repositoryRoot / "Library" / "ScriptApi" / "ScriptApi.json").string()) + "')\" />\n";
    project += "  </ItemGroup>\n";
    project += "</Project>\n";
    return WriteTextIfChanged(manifest.visualStudioProject, project, error);
}

bool WriteSolution(const ProjectDebugManifest& manifest, const std::filesystem::path& repositoryRoot, std::string& error)
{
    (void)repositoryRoot;
    const auto gameGuid = MakeGuid(manifest.projectId + ":GameScripts");
    const auto relative = [](const std::filesystem::path& from, const std::filesystem::path& to)
    {
        return to.lexically_relative(from).generic_string();
    };

    struct SolutionProject
    {
        std::string name;
        std::string typeGuid;
        std::string projectGuid;
        std::filesystem::path path;
        bool native = false;
    };
    std::vector<SolutionProject> projects;
    projects.push_back({
        "GameScripts",
        "{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}",
        gameGuid,
        manifest.visualStudioProject,
        false});

    for (const auto& engineProject : manifest.engineProjects)
    {
        const auto projectPath = manifest.engineBuildRoot / engineProject.path;
        const auto projectGuid = ReadProjectGuid(projectPath);
        if (projectGuid.empty() || !std::filesystem::is_regular_file(projectPath))
            continue;
        projects.push_back({
            engineProject.name,
            "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}",
            projectGuid,
            projectPath,
            true});
    }

    std::string solution;
    solution += "Microsoft Visual Studio Solution File, Format Version 12.00\n";
    solution += "# Visual Studio Version 17\n";
    solution += "VisualStudioVersion = 17.8.0.0\nMinimumVisualStudioVersion = 17.8.0.0\n";
    for (const auto& project : projects)
    {
        solution += "Project(\"" + project.typeGuid + "\") = \"" + project.name + "\", \"" +
            relative(manifest.visualStudioSolution.parent_path(), project.path) + "\", \"" +
            project.projectGuid + "\"\nEndProject\n";
    }
    solution += "Global\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n"
        "\t\tDebug|x64 = Debug|x64\n"
        "\t\tRelease|x64 = Release|x64\n"
        "\tEndGlobalSection\n";
    solution += "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n";
    for (const auto& project : projects)
    {
        const auto configuration = project.native ? "Debug|x64" : "Debug|Any CPU";
        const auto releaseConfiguration = project.native ? "Release|x64" : "Release|Any CPU";
        solution += "\t\t" + project.projectGuid + ".Debug|x64.ActiveCfg = " + configuration + "\n";
        solution += "\t\t" + project.projectGuid + ".Debug|x64.Build.0 = " + configuration + "\n";
        solution += "\t\t" + project.projectGuid + ".Release|x64.ActiveCfg = " + releaseConfiguration + "\n";
        solution += "\t\t" + project.projectGuid + ".Release|x64.Build.0 = " + releaseConfiguration + "\n";
    }
    solution += "\tEndGlobalSection\n\tGlobalSection(SolutionProperties) = preSolution\n\t\tHideSolutionNode = FALSE\n\tEndGlobalSection\nEndGlobal\n";
    if (!WriteTextIfChanged(manifest.visualStudioSolution, solution, error))
        return false;

    const auto solutionRoot = manifest.visualStudioSolution.parent_path();
    std::string scriptsFilter = "{\n  \"solution\": {\n    \"path\": \"Nullus.Project.sln\"\n  },\n  \"projects\": [\n    \"" +
        relative(solutionRoot, manifest.visualStudioProject) + "\"\n  ]\n}\n";
    if (!WriteTextIfChanged(manifest.visualStudioScriptsFilter, scriptsFilter, error))
        return false;

    std::string engineFilter = "{\n  \"solution\": {\n    \"path\": \"Nullus.Project.sln\"\n  },\n  \"projects\": [\n";
    bool firstEngine = true;
    for (const auto& project : projects)
    {
        if (!project.native)
            continue;
        if (!firstEngine)
            engineFilter += ",\n";
        engineFilter += "    \"" + relative(solutionRoot, project.path) + "\"";
        firstEngine = false;
    }
    engineFilter += "\n  ]\n}\n";
    return WriteTextIfChanged(manifest.visualStudioEngineFilter, engineFilter, error);
}

bool WriteWorkspace(const ProjectDebugManifest& manifest, std::string& error)
{
    Json workspace = {
        {"folders", Json::array({
            Json{{"path", manifest.projectRoot.string()}, {"name", "Game Project"}}
        })},
        {"settings", Json{
            {"nullus.projectManifest", (manifest.workspaceRoot / "Nullus.Debug.json").string()},
            {"dotnet.defaultSolution", manifest.visualStudioSolution.string()}
        }}
    };
    return WriteTextIfChanged(
        manifest.visualStudioCodeWorkspace,
        workspace.dump(4) + "\n",
        error);
}

bool ReadString(const Json& value, const char* key, std::string& output)
{
    if (!value.contains(key) || !value.at(key).is_string())
        return false;
    output = value.at(key).get<std::string>();
    return true;
}

bool ReadPath(const Json& value, const char* key, std::filesystem::path& output)
{
    std::string text;
    if (!ReadString(value, key, text))
        return false;
    output = std::filesystem::path(std::move(text));
    return true;
}

std::optional<EngineWorkspaceDescriptor> ParseEngineWorkspaceDescriptor(
    const Json& value,
    const std::filesystem::path& descriptorPath,
    std::string* errorMessage)
{
    if (!value.is_object())
    {
        if (errorMessage)
            *errorMessage = "Engine workspace descriptor is not a JSON object: " + descriptorPath.string();
        return std::nullopt;
    }

    EngineWorkspaceDescriptor descriptor;
    descriptor.schemaVersion = value.value("schemaVersion", 0u);
    descriptor.visualStudioSupported = value.value("visualStudioSupported", false);
    descriptor.generator = value.value("generator", "");
    std::string sourceRoot;
    std::string buildRoot;
    std::string cmakeExecutable;
    if (!ReadString(value, "sourceRoot", sourceRoot) || !ReadString(value, "buildRoot", buildRoot))
    {
        if (errorMessage)
            *errorMessage = "Engine workspace descriptor is missing sourceRoot or buildRoot: " + descriptorPath.string();
        return std::nullopt;
    }
    descriptor.sourceRoot = CanonicalOrNormalized(sourceRoot);
    descriptor.buildRoot = CanonicalOrNormalized(buildRoot);
    if (value.contains("cmakeExecutable") && value.at("cmakeExecutable").is_string())
    {
        cmakeExecutable = value.at("cmakeExecutable").get<std::string>();
        descriptor.cmakeExecutable = CanonicalOrNormalized(cmakeExecutable);
    }
    if (value.contains("pdb") && value.at("pdb").is_object())
    {
        std::string debugPdb;
        std::string releasePdb;
        if (ReadString(value.at("pdb"), "Debug", debugPdb))
            descriptor.debugPdb = CanonicalOrNormalized(debugPdb);
        if (ReadString(value.at("pdb"), "Release", releasePdb))
            descriptor.releasePdb = CanonicalOrNormalized(releasePdb);
    }
    descriptor.editorTarget = value.value("editorTarget", "Editor");
    if (value.contains("projects") && value.at("projects").is_array())
    {
        for (const auto& item : value.at("projects"))
        {
            if (!item.is_object() || !item.contains("name") || !item.contains("path"))
                continue;
            EngineWorkspaceProject project;
            project.name = item.value("name", "");
            project.path = item.value("path", "");
            project.sourceRoot = item.value("sourceRoot", "");
            project.kind = item.value("kind", "native");
            if (!project.name.empty() && !project.path.empty())
                descriptor.projects.push_back(std::move(project));
        }
    }
    if (descriptor.schemaVersion != 1u || descriptor.sourceRoot.empty() || descriptor.buildRoot.empty())
    {
        if (errorMessage)
            *errorMessage = "Engine workspace descriptor has an unsupported schema or missing roots: " + descriptorPath.string();
        return std::nullopt;
    }
    return descriptor;
}

bool ValidateEngineWorkspace(
    const EngineWorkspaceDescriptor& descriptor,
    std::string* errorMessage)
{
    if (!descriptor.visualStudioSupported)
    {
        if (errorMessage)
            *errorMessage = "The configured native build was not generated by a Visual Studio generator.";
        return false;
    }
    if (!std::filesystem::is_directory(descriptor.sourceRoot) ||
        !std::filesystem::is_directory(descriptor.buildRoot))
    {
        if (errorMessage)
            *errorMessage = "The EngineWorkspace descriptor points to missing source or build roots.";
        return false;
    }
    if (descriptor.projects.empty())
    {
        if (errorMessage)
            *errorMessage = "The EngineWorkspace descriptor contains no native projects.";
        return false;
    }
    for (const auto& project : descriptor.projects)
    {
        const auto projectFile = descriptor.buildRoot / project.path;
        const auto sourceRoot = descriptor.sourceRoot / project.sourceRoot;
        if (!std::filesystem::is_regular_file(projectFile) || !HasNativeSource(sourceRoot))
        {
            if (errorMessage)
                *errorMessage = "The EngineWorkspace project is missing its vcxproj or source files: " + project.name;
            return false;
        }
    }
    return true;
}
}

std::filesystem::path ResolveProjectRoot(const std::filesystem::path& projectPath)
{
    if (projectPath.empty())
        return {};
    std::error_code error;
    auto path = CanonicalOrNormalized(projectPath);
    if (std::filesystem::is_regular_file(path, error) && path.extension() == ".nullus")
        return path.parent_path();
    if (!std::filesystem::is_directory(path, error))
        return {};
    if (const auto projectFile = FindProjectFile(path); !projectFile.empty())
        return path;
    return path;
}

std::string ComputeProjectId(const std::filesystem::path& projectPath)
{
    const auto projectFile = ResolveProjectFile(projectPath);
    const auto root = ResolveProjectRoot(projectPath);
    return NLS::Core::Assets::BuildArtifactStorageFileName(
        NormalizeProjectIdentity(projectFile.empty() ? root : projectFile));
}

ProjectDebugWorkspaceResult GenerateProjectDebugWorkspace(
    const std::filesystem::path& projectPath,
    const std::filesystem::path& editorExecutable,
    const std::filesystem::path& brokerExecutable,
    const uint16_t luaPandaPort,
    const bool stopOnEntry)
{
    ProjectDebugWorkspaceResult result;
    result.manifest.projectRoot = ResolveProjectRoot(projectPath);
    if (result.manifest.projectRoot.empty())
    {
        result.errorMessage = "Unable to resolve the Nullus project root.";
        return result;
    }
    result.manifest.projectFile = ResolveProjectFile(projectPath);
    if (result.manifest.projectFile.empty())
    {
        result.errorMessage = "The project does not contain a .nullus file: " + result.manifest.projectRoot.string();
        return result;
    }
    const auto repositoryRoot = FindRepositoryRoot(result.manifest.projectRoot);
    if (repositoryRoot.empty())
    {
        result.errorMessage = "Unable to find the Nullus repository for project: " + result.manifest.projectRoot.string();
        return result;
    }

    result.manifest.projectId = ComputeProjectId(result.manifest.projectFile);
    result.manifest.protocolMajor = 2u;
    result.manifest.protocolMinor = 1u;
    result.manifest.brokerVersion = "2.0.0";
    result.manifest.requiredVsixVersion = "2.0.13";
    result.manifest.workspaceRoot = result.manifest.projectRoot / "Library" / "IDE";
    result.manifest.visualStudioSolution = result.manifest.workspaceRoot / "VisualStudio" / "Nullus.Project.sln";
    result.manifest.visualStudioScriptsFilter = result.manifest.workspaceRoot / "VisualStudio" / "Nullus.Scripts.slnf";
    result.manifest.visualStudioEngineFilter = result.manifest.workspaceRoot / "VisualStudio" / "Nullus.Engine.slnf";
    result.manifest.visualStudioProject = result.manifest.workspaceRoot / "VisualStudio" / "Nullus.GameScripts.csproj";
    result.manifest.visualStudioCodeWorkspace = result.manifest.workspaceRoot / "VSCode" / "Nullus.code-workspace";
    result.manifest.editorExecutable = CanonicalOrNormalized(editorExecutable);
    result.manifest.brokerExecutable = CanonicalOrNormalized(brokerExecutable);
    result.manifest.nativeBuildId = FileIdentity(result.manifest.editorExecutable);
    result.manifest.engineBuildId = result.manifest.nativeBuildId.empty() ? "nullus-dev" : result.manifest.nativeBuildId;
    // replace_extension mutates a path.  Keep the executable path intact in
    // the manifest; the PDB is only used to compute the native symbol
    // signature.
    auto pdbPath = result.manifest.editorExecutable;
    pdbPath.replace_extension(".pdb");
    result.manifest.nativePdbSignature = FileIdentity(pdbPath);
    result.manifest.nativeSymbolsAvailable = !result.manifest.nativePdbSignature.empty();
    result.manifest.visualStudioExtension = FindBundledVisualStudioExtension(repositoryRoot);
    result.manifest.workspaceRevision = ComputeWorkspaceRevision(result.manifest.projectRoot, repositoryRoot);

    const auto descriptorPath = FindEngineWorkspaceDescriptor(repositoryRoot);
    if (!descriptorPath.empty())
    {
        result.manifest.engineWorkspaceDescriptor = descriptorPath;
        std::string descriptorError;
        const auto descriptor = ReadEngineWorkspaceDescriptor(descriptorPath, &descriptorError);
        if (descriptor)
        {
            result.manifest.engineSourceRoot = descriptor->sourceRoot;
            result.manifest.engineBuildRoot = descriptor->buildRoot;
            result.manifest.engineSolution = descriptor->buildRoot / "Nullus.sln";
            result.manifest.cmakeExecutable = descriptor->cmakeExecutable;
            result.manifest.nativeTarget = descriptor->editorTarget;
            result.manifest.engineProjects = descriptor->projects;
            result.manifest.engineSourceAvailable = ValidateEngineWorkspace(*descriptor, &descriptorError);
        }
    }
    result.manifest.mixedDebugAvailable =
        result.manifest.engineSourceAvailable && result.manifest.nativeSymbolsAvailable &&
        !result.manifest.editorExecutable.empty();

    std::error_code error;
    std::filesystem::create_directories(result.manifest.visualStudioSolution.parent_path(), error);
    std::filesystem::create_directories(result.manifest.visualStudioCodeWorkspace.parent_path(), error);
    if (error)
    {
        result.errorMessage = "Unable to create project IDE workspace: " + error.message();
        return result;
    }

    std::string writeError;
    if (!WriteManifest(result.manifest, writeError))
    {
        result.errorMessage = writeError;
        return result;
    }
    if (!WriteProjectFile(result.manifest, repositoryRoot, writeError) ||
        !WriteSolution(result.manifest, repositoryRoot, writeError) ||
        !WriteWorkspace(result.manifest, writeError))
    {
        result.errorMessage = writeError;
        return result;
    }

    const auto visualStudio = GenerateVisualStudioConfiguration(result.manifest);
    if (!visualStudio.success)
    {
        result.errorMessage = visualStudio.errorMessage;
        return result;
    }

    const auto vscode = GenerateVSCodeConfiguration(
        result.manifest.projectRoot,
        {},
        luaPandaPort,
        stopOnEntry,
        result.manifest.workspaceRoot / "Nullus.Debug.json");
    if (!vscode.success)
    {
        result.errorMessage = vscode.errorMessage;
        return result;
    }

    result.success = true;
    return result;
}

std::optional<ProjectDebugManifest> ReadProjectDebugManifest(
    const std::filesystem::path& projectRoot,
    std::string* errorMessage)
{
    const auto path = ResolveProjectRoot(projectRoot) / "Library" / "IDE" / "Nullus.Debug.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        if (errorMessage)
            *errorMessage = "Project debug manifest does not exist: " + path.string();
        return std::nullopt;
    }
    const Json value = Json::parse(input, nullptr, false);
    if (value.is_discarded() || !value.is_object())
    {
        if (errorMessage)
            *errorMessage = "Project debug manifest is not valid JSON: " + path.string();
        return std::nullopt;
    }
    ProjectDebugManifest manifest;
    const auto schema = value.contains("schemaVersion") && value.at("schemaVersion").is_number_unsigned()
        ? value.at("schemaVersion").get<uint32_t>()
        : 0u;
    manifest.schemaVersion = schema;
    if (value.contains("protocolVersion") && value.at("protocolVersion").is_object())
    {
        manifest.protocolMajor = value.at("protocolVersion").value("major", 1u);
        manifest.protocolMinor = value.at("protocolVersion").value("minor", 0u);
    }
    else
    {
        manifest.protocolMajor = value.value("protocolMajor", 1u);
        manifest.protocolMinor = value.value("protocolMinor", 0u);
    }
    manifest.workspaceRevision = value.value("workspaceRevision", 0ull);
    manifest.engineBuildId = value.value("engineBuildId", "");
    manifest.brokerVersion = value.value("brokerVersion", "");
    manifest.requiredVsixVersion = value.value("requiredVsixVersion", "");
    manifest.nativeTarget = value.value("nativeTarget", "Editor");
    manifest.nativeConfiguration = value.value("nativeConfiguration", "Debug");
    manifest.nativeBuildId = value.value("nativeBuildId", "");
    manifest.nativePdbSignature = value.value("nativePdbSignature", "");
    manifest.engineSourceAvailable = value.value("engineSourceAvailable", false);
    manifest.nativeSymbolsAvailable = value.value("nativeSymbolsAvailable", false);
    manifest.mixedDebugAvailable = value.value("mixedDebugAvailable", false);
    const bool valid =
        ReadString(value, "projectId", manifest.projectId) &&
        ReadPath(value, "projectRoot", manifest.projectRoot) &&
        ReadPath(value, "projectFile", manifest.projectFile) &&
        ReadPath(value, "workspaceRoot", manifest.workspaceRoot) &&
        ReadPath(value, "visualStudioSolution", manifest.visualStudioSolution) &&
        ReadPath(value, "visualStudioProject", manifest.visualStudioProject) &&
        ReadPath(value, "visualStudioCodeWorkspace", manifest.visualStudioCodeWorkspace) &&
        (!value.contains("visualStudioExtension") || ReadPath(value, "visualStudioExtension", manifest.visualStudioExtension)) &&
        ReadPath(value, "editorExecutable", manifest.editorExecutable) &&
        ReadPath(value, "brokerExecutable", manifest.brokerExecutable);
    if (value.contains("visualStudioScriptsFilter"))
        ReadPath(value, "visualStudioScriptsFilter", manifest.visualStudioScriptsFilter);
    if (value.contains("visualStudioEngineFilter"))
        ReadPath(value, "visualStudioEngineFilter", manifest.visualStudioEngineFilter);
    if (value.contains("engineWorkspaceDescriptor"))
        ReadPath(value, "engineWorkspaceDescriptor", manifest.engineWorkspaceDescriptor);
    if (value.contains("engineSourceRoot"))
        ReadPath(value, "engineSourceRoot", manifest.engineSourceRoot);
    if (value.contains("engineBuildRoot"))
        ReadPath(value, "engineBuildRoot", manifest.engineBuildRoot);
    if (value.contains("engineSolution"))
        ReadPath(value, "engineSolution", manifest.engineSolution);
    if (value.contains("cmakeExecutable"))
        ReadPath(value, "cmakeExecutable", manifest.cmakeExecutable);
    if (value.contains("engineProjects") && value.at("engineProjects").is_array())
    {
        for (const auto& item : value.at("engineProjects"))
        {
            if (!item.is_object())
                continue;
            EngineWorkspaceProject project;
            project.name = item.value("name", "");
            project.path = item.value("path", "");
            project.sourceRoot = item.value("sourceRoot", "");
            project.kind = item.value("kind", "native");
            if (!project.name.empty() && !project.path.empty())
                manifest.engineProjects.push_back(std::move(project));
        }
    }
    if (!valid || (manifest.schemaVersion != 1u && manifest.schemaVersion != 2u && manifest.schemaVersion != 3u) || manifest.projectId.empty() || manifest.projectRoot.empty())
    {
        if (errorMessage)
            *errorMessage = "Project debug manifest has an unsupported schema or missing project identity.";
        return std::nullopt;
    }
    return manifest;
}

std::optional<EngineWorkspaceDescriptor> ReadEngineWorkspaceDescriptor(
    const std::filesystem::path& descriptorPath,
    std::string* errorMessage)
{
    std::ifstream input(descriptorPath, std::ios::binary);
    if (!input)
    {
        if (errorMessage)
            *errorMessage = "Engine workspace descriptor does not exist: " + descriptorPath.string();
        return std::nullopt;
    }
    const auto value = Json::parse(input, nullptr, false);
    if (value.is_discarded())
    {
        if (errorMessage)
            *errorMessage = "Engine workspace descriptor is not valid JSON: " + descriptorPath.string();
        return std::nullopt;
    }
    return ParseEngineWorkspaceDescriptor(value, descriptorPath, errorMessage);
}
}
