using System;
using System.Collections.Generic;
using System.Linq;
using EnvDTE;
using EnvDTE80;
using Microsoft.VisualStudio.Shell;

namespace Nullus.ScriptDebugger;

// Mirrors the Editor's bounded diagnostic stream into the native VS Error
// List. Build diagnostics are replaced by the newest build; runtime failures
// remain until the project session ends or the user clears the list.
internal sealed class NullusDiagnosticsService : ErrorListProvider
{
    private readonly List<IDictionary<string, object>> _runtimeDiagnostics = new();
    private IDictionary<string, object>? _buildDiagnostic;

    public NullusDiagnosticsService(IServiceProvider serviceProvider)
        : base(serviceProvider)
    {
    }

    public void AddEvents(IEnumerable<IDictionary<string, object>> events)
    {
        foreach (var value in events)
        {
            var type = StringValue(value, "type");
            if (type == "Build")
            {
                _buildDiagnostic = BoolValue(value, "succeeded") ? null : value;
            }
            else if (type == "RuntimeDiagnostic")
            {
                _runtimeDiagnostics.Add(value);
                if (_runtimeDiagnostics.Count > 256)
                    _runtimeDiagnostics.RemoveAt(0);
            }
        }
        RebuildTasks();
    }

    public void ClearRuntimeDiagnostics()
    {
        _runtimeDiagnostics.Clear();
        RebuildTasks();
    }

    private void RebuildTasks()
    {
        Tasks.Clear();
        if (_buildDiagnostic != null)
            AddTask(_buildDiagnostic, TaskCategory.BuildCompile);
        foreach (var diagnostic in _runtimeDiagnostics)
            AddTask(diagnostic, TaskCategory.User);
        if (Tasks.Count > 0)
            BringToFront();
    }

    private void AddTask(IDictionary<string, object> value, TaskCategory category)
    {
        var source = StringValue(value, "sourcePath");
        var line = Math.Max(0, IntValue(value, "line") - 1);
        var column = Math.Max(0, IntValue(value, "column") - 1);
        var language = StringValue(value, "language");
        var message = StringValue(value, "message");
        var text = string.IsNullOrWhiteSpace(language) ? message : "[" + language + "] " + message;
        var task = new ErrorTask
        {
            Category = category,
            ErrorCategory = TaskErrorCategory.Error,
            Document = source,
            Line = line,
            Column = column,
            Text = text
        };
        task.Navigate += (_, _) => OpenSource(source, line, column);
        Tasks.Add(task);
    }

    private void OpenSource(string path, int line, int column)
    {
        if (string.IsNullOrWhiteSpace(path))
            return;
        ThreadHelper.ThrowIfNotOnUIThread();
        var dte = GetService(typeof(DTE)) as DTE2;
        if (dte == null)
            return;
        var document = dte.ItemOperations.OpenFile(path);
        if (document?.Selection is TextSelection selection)
        {
            selection.GotoLine(Math.Max(1, line + 1), false);
            selection.MoveToLineAndOffset(Math.Max(1, line + 1), Math.Max(1, column + 1), false);
        }
    }

    private static string StringValue(IDictionary<string, object> value, string key) =>
        value.TryGetValue(key, out var result) ? result?.ToString() ?? string.Empty : string.Empty;

    private static int IntValue(IDictionary<string, object> value, string key) =>
        value.TryGetValue(key, out var result) ? Convert.ToInt32(result) : 0;

    private static bool BoolValue(IDictionary<string, object> value, string key) =>
        value.TryGetValue(key, out var result) && Convert.ToBoolean(result);
}
