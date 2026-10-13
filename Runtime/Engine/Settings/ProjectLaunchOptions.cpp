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
        if (projectFile.IsKeyExisting(ProjectLaunchOptionKeys::RhiThreadMode))
        {
            const auto value = projectFile.GetOrDefault<std::string>(ProjectLaunchOptionKeys::RhiThreadMode, "");
            const auto mode = Render::Settings::ParseRhiThreadModePreference(value);
            if (!mode.has_value())
            {
                if (p_outError != nullptr)
                    *p_outError = "Invalid rhi_thread_mode: " + value;
                return options;
            }
            options.rhiThreadMode = *mode;
            options.hasRhiThreadMode = true;
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

    const char* const rhiMode = Render::Settings::ToConfigString(p_options.rhiThreadMode);
    if (rhiMode == nullptr)
    {
        if (p_outError != nullptr)
            *p_outError = "Invalid RHI thread mode preference.";
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
        projectFile.Remove("enable_threaded_rendering");
        store(ProjectLaunchOptionKeys::RhiThreadMode, std::string(rhiMode));
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
