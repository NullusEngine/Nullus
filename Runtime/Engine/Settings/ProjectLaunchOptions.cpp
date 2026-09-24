#include <Settings/ProjectLaunchOptions.h>

#include <Core/Filesystem/IniFile.h>

namespace NLS::Engine::Settings
{
ProjectLaunchOptions ReadProjectLaunchOptions(
    const std::filesystem::path& p_projectFilePath,
    std::string* p_outError)
{
    if (p_outError != nullptr)
        p_outError->clear();

    ProjectLaunchOptions options;
    std::error_code error;
    if (p_projectFilePath.empty() || !std::filesystem::exists(p_projectFilePath, error) || error)
        return options;

    try
    {
        Filesystem::IniFile projectFile(p_projectFilePath.string());
        if (projectFile.IsKeyExisting(ProjectLaunchOptionKeys::VerticalSynchronization))
        {
            options.verticalSynchronization = projectFile.GetOrDefault<bool>(
                ProjectLaunchOptionKeys::VerticalSynchronization, options.verticalSynchronization);
            options.hasVerticalSynchronization = true;
        }
        if (projectFile.IsKeyExisting(ProjectLaunchOptionKeys::ThreadedRendering))
        {
            options.threadedRendering = projectFile.GetOrDefault<bool>(
                ProjectLaunchOptionKeys::ThreadedRendering, options.threadedRendering);
            options.hasThreadedRendering = true;
        }
        if (projectFile.IsKeyExisting(ProjectLaunchOptionKeys::RenderDocStartupCaptureFrames))
        {
            options.renderDocStartupCaptureFrames = projectFile.GetOrDefault<int>(
                ProjectLaunchOptionKeys::RenderDocStartupCaptureFrames, options.renderDocStartupCaptureFrames);
            options.hasRenderDocStartupCaptureFrames = true;
        }
    }
    catch (const std::exception& exception)
    {
        if (p_outError != nullptr)
            *p_outError = exception.what();
        return options;
    }

    if (options.renderDocStartupCaptureFrames < 0)
        options.renderDocStartupCaptureFrames = 0;

    return options;
}

bool WriteProjectLaunchOptions(
    const std::filesystem::path& p_projectFilePath,
    const ProjectLaunchOptions& p_options,
    std::string* p_outError)
{
    if (p_outError != nullptr)
        p_outError->clear();

    if (p_projectFilePath.empty())
    {
        if (p_outError != nullptr)
            *p_outError = "No project file path was supplied.";
        return false;
    }

    try
    {
        Filesystem::IniFile projectFile(p_projectFilePath.string());
        const auto store = [&projectFile](const char* p_key, const auto& p_value)
        {
            if (!projectFile.Set(p_key, p_value))
                projectFile.Add(p_key, p_value);
        };

        store(ProjectLaunchOptionKeys::VerticalSynchronization, p_options.verticalSynchronization);
        store(ProjectLaunchOptionKeys::ThreadedRendering, p_options.threadedRendering);
        store(
            ProjectLaunchOptionKeys::RenderDocStartupCaptureFrames,
            p_options.renderDocStartupCaptureFrames < 0 ? 0 : p_options.renderDocStartupCaptureFrames);
        projectFile.Rewrite();
        return true;
    }
    catch (const std::exception& exception)
    {
        if (p_outError != nullptr)
            *p_outError = exception.what();
        return false;
    }
}
}
