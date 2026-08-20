using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio.ProjectSystem;
using Microsoft.VisualStudio.ProjectSystem.Debug;
using Microsoft.VisualStudio.ProjectSystem.VS.Debug;
using Microsoft.VisualStudio.Shell.Interop;

namespace Nullus.ScriptDebugger;

// launchSettings.json is owned by VS's LaunchProfilesDebugLaunchProvider.
// This provider handles only profiles whose commandName is NullusEditor and
// returns an AlreadyRunning target for the PID validated by the Broker.
[Export(typeof(IDebugProfileLaunchTargetsProvider))]
[AppliesTo("LaunchProfiles")]
[Order(0)]
internal sealed class NullusDebugLaunchProvider :
    IDebugProfileLaunchTargetsProvider,
    IDebugProfileLaunchTargetsProvider2,
    IDebugProfileLaunchTargetsProvider3,
    IDebugProfileLaunchTargetsProvider4
{
    private static readonly Guid CoreClrDebugEngine = new("2e36f1d4-b23c-435d-ab41-18e608940038");
    private static readonly Guid NativeDebugEngine = new("3b476d35-a401-11d2-aad4-00c04f990171");
    // C# language service GUID registered by VS 2022.  The CoreCLR engine can
    // attach without it, but VS then leaves C# source breakpoints unprocessed.
    private static readonly Guid CSharpLanguage = new("694dd9b6-b865-4c5b-ad85-86356e9c88dc");
    private static int s_playAfterAttach;
    private static int s_mixedDebug;
    private readonly ConfiguredProject _configuredProject;
    private int _playDispatched;

    internal static void RequestPlayAfterAttach() => s_playAfterAttach = 1;
    internal static void RequestMixedDebug() => s_mixedDebug = 1;

    [ImportingConstructor]
    public NullusDebugLaunchProvider(ConfiguredProject configuredProject)
    {
        _configuredProject = configuredProject ?? throw new ArgumentNullException(nameof(configuredProject));
    }

    public bool SupportsProfile(ILaunchProfile profile)
    {
        return profile != null &&
            string.Equals(profile.CommandName, "NullusEditor", StringComparison.Ordinal);
    }

    public Task<bool> CanBeStartupProjectAsync(DebugLaunchOptions launchOptions, ILaunchProfile profile)
    {
        return Task.FromResult(SupportsProfile(profile));
    }

    public Task<IReadOnlyList<IDebugLaunchSettings>> QueryDebugTargetsAsync(
        DebugLaunchOptions launchOptions,
        ILaunchProfile profile)
    {
        return QueryDebugTargetsCoreAsync(launchOptions, profile);
    }

    public Task<IReadOnlyList<IDebugLaunchSettings>> QueryDebugTargetsForDebugLaunchAsync(
        DebugLaunchOptions launchOptions,
        ILaunchProfile profile)
    {
        return QueryDebugTargetsCoreAsync(launchOptions, profile);
    }

    public Task OnBeforeLaunchAsync(DebugLaunchOptions launchOptions, ILaunchProfile profile)
    {
        Interlocked.Exchange(ref _playDispatched, 0);
        return Task.CompletedTask;
    }

    public Task OnBeforeLaunchAsync(
        DebugLaunchOptions launchOptions,
        ILaunchProfile profile,
        IReadOnlyList<IDebugLaunchSettings> debugLaunchSettings)
    {
        Interlocked.Exchange(ref _playDispatched, 0);
        return Task.CompletedTask;
    }

    public Task OnAfterLaunchAsync(DebugLaunchOptions launchOptions, ILaunchProfile profile)
    {
        return DispatchPlayAfterAttachAsync(profile);
    }

    public Task OnAfterLaunchAsync(
        DebugLaunchOptions launchOptions,
        ILaunchProfile profile,
        IReadOnlyList<VsDebugTargetProcessInfo> processInfos)
    {
        return DispatchPlayAfterAttachAsync(profile);
    }

    private async Task<IReadOnlyList<IDebugLaunchSettings>> QueryDebugTargetsCoreAsync(
        DebugLaunchOptions launchOptions,
        ILaunchProfile profile)
    {
        if (!SupportsProfile(profile))
            throw new InvalidOperationException("The selected launch profile is not a NullusEditor profile.");

        Interlocked.Exchange(ref _playDispatched, 0);
        var manifestPath = GetManifestPath(profile);
        var profileMode = GetStringSetting(profile, "nullusMode") ?? "managed";
        // The solution configuration is the source of truth.  Older
        // generated launch profiles contain a fixed nullusConfiguration, but
        // that value must not make a Release solution start the Debug Editor.
        var configuration = GetActiveConfiguration() ??
            GetStringSetting(profile, "nullusConfiguration") ?? "Debug";
        var commandRequestedMixed = Interlocked.Exchange(ref s_mixedDebug, 0) != 0;
        WriteLaunchLog($"begin manifest={manifestPath} profile={profileMode} configuration={configuration}");
        var mixedRequested = commandRequestedMixed || string.Equals(profileMode, "mixed", StringComparison.OrdinalIgnoreCase);
        var prepareAction = mixedRequested
            ? "prepare-mixed"
            : "prepare";
        var brokerResult = await NullusBrokerClient.RunAsync(
            manifestPath,
            prepareAction,
            default,
            configuration).ConfigureAwait(false);
        if (!brokerResult.Ok)
            throw new InvalidOperationException(brokerResult.Error ?? "Nullus C# Debug preparation failed.");
        if (!brokerResult.Data.TryGetValue("processId", out var processValue))
            throw new InvalidOperationException("NullusDebugBroker did not return an Editor process ID.");
        if (!brokerResult.Data.TryGetValue("editorExecutable", out var executableValue) ||
            executableValue is not string executable ||
            !Path.IsPathRooted(executable) ||
            !File.Exists(executable))
        {
            throw new InvalidOperationException("NullusDebugBroker did not return a valid Editor executable.");
        }

        var settings = new DebugLaunchSettings(launchOptions)
        {
            LaunchOperation = DebugLaunchOperation.AlreadyRunning,
            LaunchDebugEngineGuid = CoreClrDebugEngine,
            ProcessId = Convert.ToInt32(processValue),
            // VS validates bstrExe even for an AlreadyRunning target.  Leaving
            // it empty reaches the debugger launch service as an invalid target
            // and results in E_INVALIDARG (0x80070057).
            Executable = executable,
            CurrentDirectory = Path.GetDirectoryName(manifestPath),
            SendToOutputWindow = true,
            ProcessLanguageGuid = CSharpLanguage,
        };
        if (mixedRequested)
            settings.AdditionalDebugEngines.Add(NativeDebugEngine);
        WriteLaunchLog($"manifest={manifestPath}\nprocessId={settings.ProcessId}\nexecutable={settings.Executable}\nconfiguration={configuration}\nengine={settings.LaunchDebugEngineGuid}\nlanguage={settings.ProcessLanguageGuid}\nadditional={string.Join(",", settings.AdditionalDebugEngines)}");
        return new[] { settings };
    }

    private static void WriteLaunchLog(string message)
    {
        try
        {
            File.AppendAllText(
                Path.Combine(Path.GetTempPath(), "Nullus.ScriptDebugger.log"),
                $"{DateTime.UtcNow:O}\n{message}\n");
        }
        catch
        {
            // Diagnostics must never prevent a debug launch.
        }
    }

    private async Task DispatchPlayAfterAttachAsync(ILaunchProfile profile)
    {
        var commandRequestedPlay = Interlocked.Exchange(ref s_playAfterAttach, 0) != 0;
        if (!commandRequestedPlay && !GetBooleanSetting(profile, "nullusPlayAfterAttach"))
            return;
        if (Interlocked.Exchange(ref _playDispatched, 1) != 0)
            return;

        var manifestPath = GetManifestPath(profile);
        var startedAt = Stopwatch.GetTimestamp();
        WriteLaunchLog($"attach-and-play dispatch begin manifest={manifestPath}");
        var result = await NullusBrokerClient.RunAsync(manifestPath, "play", default).ConfigureAwait(false);
        var elapsedMs = (Stopwatch.GetTimestamp() - startedAt) * 1000.0 / Stopwatch.Frequency;
        WriteLaunchLog($"attach-and-play dispatch complete ok={result.Ok} elapsedMs={elapsedMs:F1}");
        if (!result.Ok)
            throw new InvalidOperationException(result.Error ?? "Nullus Editor did not enter Play after the debugger attached.");
    }

    private string GetManifestPath(ILaunchProfile profile)
    {
        var profileManifest = GetStringSetting(profile, "nullusManifest");
        if (!string.IsNullOrWhiteSpace(profileManifest))
        {
            var fullManifestPath = Path.GetFullPath(profileManifest);
            if (!File.Exists(fullManifestPath))
                throw new InvalidOperationException("The Nullus launch profile manifest does not exist: " + fullManifestPath);
            return fullManifestPath;
        }

        var projectPath = _configuredProject.UnconfiguredProject.FullPath;
        if (string.IsNullOrWhiteSpace(projectPath))
            throw new InvalidOperationException("The Nullus project workspace path is unavailable. Reload the project and try again.");

        try
        {
            return NullusBrokerClient.FindManifest(projectPath);
        }
        catch (InvalidOperationException exception)
        {
            throw new InvalidOperationException(
                "This is not a generated Nullus project workspace. Open <Project>\\Library\\IDE\\VisualStudio\\Nullus.Project.sln, then retry F5. " +
                exception.Message,
                exception);
        }
    }

    private string? GetActiveConfiguration()
    {
        var name = _configuredProject.ProjectConfiguration?.Name;
        if (string.IsNullOrWhiteSpace(name))
            return null;

        // CPS normally exposes "Debug|x64".  Accept a configuration-only
        // value as well so this remains compatible with older project-system
        // versions and generated project files.
        var configurationName = name!;
        var separator = configurationName.IndexOf('|');
        var configuration = (separator >= 0
            ? configurationName.Substring(0, separator)
            : configurationName).Trim();
        if (configuration.Equals("Debug", StringComparison.OrdinalIgnoreCase))
            return "Debug";
        if (configuration.Equals("Release", StringComparison.OrdinalIgnoreCase))
            return "Release";
        return null;
    }

    private static string? GetStringSetting(ILaunchProfile profile, string name)
    {
        var settings = profile.OtherSettings;
        return settings != null && settings.TryGetValue(name, out var value) ? value?.ToString() : null;
    }

    private static bool GetBooleanSetting(ILaunchProfile profile, string name)
    {
        var settings = profile.OtherSettings;
        if (settings == null || !settings.TryGetValue(name, out var value) || value == null)
            return false;
        if (value is bool boolean)
            return boolean;
        return bool.TryParse(value.ToString(), out var parsed) && parsed;
    }

}
