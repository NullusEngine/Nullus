using System;
using System.Collections.Generic;
using System.ComponentModel.Design;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using EnvDTE;
using EnvDTE80;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;

namespace Nullus.ScriptDebugger;

[PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true)]
[InstalledProductRegistration("Nullus Script Debugger", "Project-scoped Nullus C# debugging", "2.0")]
[ProvideMenuResource("NullusDebugPackage.CTMENU", 1)]
[ProvideAutoLoad(UIContextGuids80.SolutionExists, PackageAutoLoadFlags.BackgroundLoad)]
[global::System.Runtime.InteropServices.Guid("7df2b87c-86b1-4de3-b40e-6baaa5c19c41")]
public sealed class NullusDebugPackage : AsyncPackage
{
    private static readonly Guid CommandSet = new("6e0eb88c-5cb2-4ff6-9f9d-df3d7b03c8f0");
    private Timer? _eventTimer;
    private ulong _eventSequence;
    private NullusDiagnosticsService? _diagnostics;
    private string? _eventManifest;
    private int _eventPollInProgress;

    protected override async Task InitializeAsync(CancellationToken cancellationToken, IProgress<ServiceProgressData> progress)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var commands = await GetServiceAsync(typeof(IMenuCommandService)) as OleMenuCommandService;
        if (commands == null)
            return;

        _diagnostics = new NullusDiagnosticsService(this);
        _eventTimer = new Timer(_ => _ = PollEventsAsync(), null, 500, 1000);

        AddCommand(commands, 0x0100, (_, _) => _ = RunProviderAsync(false));
        AddCommand(commands, 0x0101, (_, _) => _ = RunProviderAsync(true));
        AddCommand(commands, 0x0102, (_, _) => _ = RunBrokerActionAsync("focus"));
        AddCommand(commands, 0x0103, (_, _) => _ = RunBrokerActionAsync("build"));
        AddCommand(commands, 0x0104, (_, _) => _ = RunBrokerActionAsync("play"));
        AddCommand(commands, 0x0105, (_, _) => _ = RunBrokerActionAsync("pause"));
        AddCommand(commands, 0x0106, (_, _) => _ = RunBrokerActionAsync("resume"));
        AddCommand(commands, 0x0107, (_, _) => _ = RunBrokerActionAsync("stop"));
        AddCommand(commands, 0x0108, (_, _) => _ = RunMixedProviderAsync());
        AddCommand(commands, 0x0109, (_, _) => _diagnostics?.ClearRuntimeDiagnostics());
        AddCommand(commands, 0x010A, (_, _) => _ = RunBrokerActionAsync("build-native", "Debug"));
    }

    private async Task PollEventsAsync()
    {
        if (Interlocked.Exchange(ref _eventPollInProgress, 1) != 0)
            return;
        try
        {
            await JoinableTaskFactory.SwitchToMainThreadAsync();
            var dte = await GetServiceAsync(typeof(DTE)) as DTE2;
            var solution = dte?.Solution?.FullName;
            if (string.IsNullOrWhiteSpace(solution))
                return;
            var manifest = NullusBrokerClient.FindManifest(solution!);
            if (!string.Equals(_eventManifest, manifest, StringComparison.OrdinalIgnoreCase))
            {
                _eventManifest = manifest;
                _eventSequence = 0;
                _diagnostics?.ClearRuntimeDiagnostics();
            }
            var result = await NullusBrokerClient.ReadEventsAsync(
                manifest, _eventSequence, 0, CancellationToken.None).ConfigureAwait(false);
            if (!result.Ok || result.Events.Count == 0)
                return;
            _eventSequence = Math.Max(_eventSequence, result.NextSequence);
            await JoinableTaskFactory.SwitchToMainThreadAsync();
            _diagnostics?.AddEvents(result.Events);
            UpdateStatusBar(result.Events);
        }
        catch
        {
            // The Error List must never make VS noisy while a project is
            // closed or the Editor is still starting. The next tick retries.
        }
        finally
        {
            Interlocked.Exchange(ref _eventPollInProgress, 0);
        }
    }

    private void UpdateStatusBar(IEnumerable<IDictionary<string, object>> events)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var latest = events.LastOrDefault();
        if (latest == null)
            return;
        var type = latest.TryGetValue("type", out var typeValue) ? typeValue?.ToString() : null;
        var state = latest.TryGetValue("state", out var stateValue) ? stateValue?.ToString() : null;
        var text = string.IsNullOrWhiteSpace(state) ? "Nullus: " + type : "Nullus: " + type + " - " + state;
        if (GetService(typeof(SVsStatusbar)) is IVsStatusbar statusbar)
            statusbar.SetText(text);
    }

    private static void AddCommand(OleMenuCommandService commands, int id, EventHandler handler)
    {
        commands.AddCommand(new OleMenuCommand(handler, new CommandID(CommandSet, id)));
    }

    private async Task RunProviderAsync(bool playAfterAttach)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync();
        try
        {
            var dte = await GetServiceAsync(typeof(DTE)) as DTE2;
            if (dte == null || string.IsNullOrWhiteSpace(dte.Solution?.FullName))
                throw new InvalidOperationException("Open a generated Nullus project workspace first.");
            // VS Debug.Start resolves the CPS NullusEditor provider. This
            // package never calls EnvDTE.Process.Attach. The provider sends
            // EnterPlay only after VS reports the target attached.
            if (playAfterAttach)
                NullusDebugLaunchProvider.RequestPlayAfterAttach();
            dte.ExecuteCommand("Debug.Start");
        }
        catch (Exception exception)
        {
            await ShowErrorAsync(exception.Message).ConfigureAwait(true);
        }
    }

    private async Task RunMixedProviderAsync()
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync();
        try
        {
            var dte = await GetServiceAsync(typeof(DTE)) as DTE2;
            if (dte == null || string.IsNullOrWhiteSpace(dte.Solution?.FullName))
                throw new InvalidOperationException("Open a generated Nullus project workspace first.");
            NullusDebugLaunchProvider.RequestMixedDebug();
            dte.ExecuteCommand("Debug.Start");
        }
        catch (Exception exception)
        {
            await ShowErrorAsync(exception.Message).ConfigureAwait(true);
        }
    }

    private async Task RunBrokerActionAsync(string action, string? configuration = null)
    {
        try
        {
            await JoinableTaskFactory.SwitchToMainThreadAsync();
            var dte = await GetServiceAsync(typeof(DTE)) as DTE2;
            var solution = dte?.Solution?.FullName;
            if (string.IsNullOrWhiteSpace(solution))
                throw new InvalidOperationException("Open a generated Nullus project workspace first.");
            var manifest = NullusBrokerClient.FindManifest(solution!);
            var result = await NullusBrokerClient.RunAsync(manifest, action, CancellationToken.None, configuration).ConfigureAwait(true);
            if (!result.Ok)
                throw new InvalidOperationException(result.Error ?? $"Nullus Broker action '{action}' failed.");
        }
        catch (Exception exception)
        {
            await ShowErrorAsync(exception.Message).ConfigureAwait(true);
        }
    }

    private async Task ShowErrorAsync(string message)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync();
        VsShellUtilities.ShowMessageBox(this, message, "Nullus Debugging", OLEMSGICON.OLEMSGICON_CRITICAL,
            OLEMSGBUTTON.OLEMSGBUTTON_OK, OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _eventTimer?.Dispose();
            _eventTimer = null;
            _diagnostics?.Dispose();
            _diagnostics = null;
        }
        base.Dispose(disposing);
    }
}
