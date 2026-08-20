using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace Nullus.ScriptDebugger;

internal sealed class NullusBrokerResult
{
    public bool Ok { get; set; }
    public string? Error { get; set; }
    public IDictionary<string, object> Data { get; set; } = new Dictionary<string, object>();
}

internal sealed class NullusEventResult
{
    public bool Ok { get; set; }
    public string? Error { get; set; }
    public ulong NextSequence { get; set; }
    public List<IDictionary<string, object>> Events { get; } = new();
}

internal static class NullusBrokerClient
{
    private static readonly JavaScriptSerializer Serializer = new();

    public static string FindManifest(string solutionPath)
    {
        if (string.IsNullOrWhiteSpace(solutionPath))
            throw new InvalidOperationException("The Visual Studio project workspace path is empty.");

        // CPS normally supplies the project file path, while DTE supplies the
        // solution file path. Accept both forms (and a directory) so the
        // provider does not depend on which VS surface initiated F5.
        var fullPath = Path.GetFullPath(solutionPath);
        DirectoryInfo? current;
        if (Directory.Exists(fullPath))
        {
            current = new DirectoryInfo(fullPath);
        }
        else
        {
            current = new FileInfo(fullPath).Directory;
        }

        if (current == null)
            throw new InvalidOperationException("The Visual Studio workspace path has no directory.");

        while (current != null)
        {
            var manifest = Path.Combine(current.FullName, "Nullus.Debug.json");
            if (File.Exists(manifest))
                return manifest;
            current = current.Parent;
        }
        throw new InvalidOperationException("Nullus.Debug.json was not found next to the project workspace. Regenerate the project workspace from the Editor.");
    }

    public static async Task<NullusBrokerResult> RunAsync(
        string manifestPath,
        string action,
        CancellationToken cancellationToken,
        string? configuration = null)
    {
        var values = Serializer.DeserializeObject(File.ReadAllText(manifestPath)) as IDictionary<string, object>
            ?? throw new InvalidOperationException("Nullus.Debug.json is invalid.");
        if (!values.TryGetValue("brokerExecutable", out var brokerValue) || brokerValue is not string broker || !Path.IsPathRooted(broker) || !File.Exists(broker))
            throw new InvalidOperationException("Nullus.Debug.json does not contain an absolute Broker path.");

        using var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = broker,
                Arguments = "--manifest " + Quote(manifestPath) + " --action " + Quote(action) +
                    (string.IsNullOrWhiteSpace(configuration) ? string.Empty : " --configuration " + Quote(configuration!)) +
                    " --timeout-ms 120000",
                WorkingDirectory = Path.GetDirectoryName(manifestPath) ?? Environment.CurrentDirectory,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true
            }
        };
        if (!process.Start())
            throw new InvalidOperationException("Unable to start NullusDebugBroker.");
        var outputTask = process.StandardOutput.ReadToEndAsync();
        var errorTask = process.StandardError.ReadToEndAsync();
        await Task.Run(() => process.WaitForExit(), cancellationToken).ConfigureAwait(false);
        var output = await outputTask.ConfigureAwait(false);
        var error = await errorTask.ConfigureAwait(false);
        var line = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries).LastOrDefault();
        var data = !string.IsNullOrWhiteSpace(line)
            ? Serializer.DeserializeObject(line) as IDictionary<string, object>
            : null;
        var normalizedData = data ?? new Dictionary<string, object>();
        // Broker protocol v2 wraps command payloads in `data`. Keep the
        // flattened view for the CPS launch provider and older VSIX builds.
        if (data != null && data.TryGetValue("data", out var payload) &&
            payload is IDictionary<string, object> payloadValues)
        {
            foreach (var pair in payloadValues)
                normalizedData[pair.Key] = pair.Value;
        }
        if (process.ExitCode != 0)
        {
            var failureText = string.IsNullOrWhiteSpace(error) ? output.Trim() : error.Trim();
            IDictionary<string, object>? failure = null;
            try
            {
                failure = Serializer.DeserializeObject(failureText) as IDictionary<string, object>;
            }
            catch
            {
                // Preserve the raw process error when an older Broker or an
                // OS launch failure did not emit the structured payload.
            }
            if (failure != null)
            {
                if (failure.TryGetValue("code", out var code))
                    normalizedData["errorCode"] = code;
                if (failure.TryGetValue("debuggerType", out var debuggerType))
                    normalizedData["debuggerType"] = debuggerType;
                if (failure.TryGetValue("processId", out var processId))
                    normalizedData["occupiedProcessId"] = processId;
                if (failure.TryGetValue("suggestion", out var suggestion))
                    normalizedData["suggestion"] = suggestion;
                if (failure.TryGetValue("error", out var message) && message != null)
                    failureText = message.ToString() ?? failureText;
            }
            return new NullusBrokerResult { Ok = false, Error = failureText, Data = normalizedData };
        }
        return new NullusBrokerResult
        {
            Ok = data == null || !data.TryGetValue("ok", out var ok) || Convert.ToBoolean(ok),
            Error = data != null && data.TryGetValue("error", out var value) ? value?.ToString() : null,
            Data = normalizedData
        };
    }

    public static async Task<NullusEventResult> ReadEventsAsync(
        string manifestPath,
        ulong afterSequence,
        int waitMs,
        CancellationToken cancellationToken)
    {
        var values = Serializer.DeserializeObject(File.ReadAllText(manifestPath)) as IDictionary<string, object>
            ?? throw new InvalidOperationException("Nullus.Debug.json is invalid.");
        if (!values.TryGetValue("brokerExecutable", out var brokerValue) || brokerValue is not string broker ||
            !Path.IsPathRooted(broker) || !File.Exists(broker))
            throw new InvalidOperationException("Nullus.Debug.json does not contain an absolute Broker path.");

        using var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = broker,
                Arguments = "--manifest " + Quote(manifestPath) + " --action watch-events --after " +
                    afterSequence.ToString() + " --wait-ms " + Math.Max(0, Math.Min(30000, waitMs)).ToString() +
                    " --timeout-ms " + Math.Max(2000, Math.Min(32000, waitMs + 2000)).ToString(),
                WorkingDirectory = Path.GetDirectoryName(manifestPath) ?? Environment.CurrentDirectory,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true
            }
        };
        if (!process.Start())
            throw new InvalidOperationException("Unable to start NullusDebugBroker.");
        var outputTask = process.StandardOutput.ReadToEndAsync();
        var errorTask = process.StandardError.ReadToEndAsync();
        await Task.Run(() => process.WaitForExit(), cancellationToken).ConfigureAwait(false);
        var output = await outputTask.ConfigureAwait(false);
        var error = await errorTask.ConfigureAwait(false);
        var result = new NullusEventResult { Ok = process.ExitCode == 0 };
        foreach (var line in output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
        {
            if (!(Serializer.DeserializeObject(line) is IDictionary<string, object> value))
                continue;
            if (value.TryGetValue("sequence", out var sequence))
            {
                var current = Convert.ToUInt64(sequence);
                if (current > result.NextSequence)
                    result.NextSequence = current;
            }
            result.Events.Add(value);
        }
        if (!result.Ok)
            result.Error = string.IsNullOrWhiteSpace(error) ? output.Trim() : error.Trim();
        return result;
    }

    private static string Quote(string value) => "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
}
