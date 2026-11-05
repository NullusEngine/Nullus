using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Text.Json;
using CppAst;

internal static partial class MetaParserTool
{
    private static StringComparer PathComparer => OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    private sealed record ParseExecutionSettings(string Mode, int Jobs)
    {
        public static ParseExecutionSettings FromConfig(PrecompileParams config)
        {
            var mode = (Environment.GetEnvironmentVariable("NLS_META_PARSER_MODE") ?? config.ParseMode)
                .Trim().ToLowerInvariant();
            if (mode is not ("serial" or "threads" or "processes"))
                throw new ArgumentException("ParseMode must be serial, threads, or processes.");

            var jobs = config.ParseJobs;
            var environmentJobs = Environment.GetEnvironmentVariable("NLS_META_PARSER_JOBS");
            if (environmentJobs is not null && !int.TryParse(environmentJobs, NumberStyles.None, CultureInfo.InvariantCulture, out jobs))
                throw new ArgumentException("NLS_META_PARSER_JOBS must be a nonnegative integer.");
            if (jobs < 0)
                throw new ArgumentException("ParseJobs must be a nonnegative integer (0 selects automatic concurrency).");
            return new ParseExecutionSettings(mode, jobs);
        }

        public int ResolveJobs(int headerCount)
        {
            if (Mode == "serial")
                return 1;
            // Modules may themselves be built in parallel by CMake/MSBuild.
            // Keep the automatic per-module budget conservative.
            var automaticJobs = Math.Clamp(Environment.ProcessorCount / 2, 1, 4);
            return Math.Clamp(Jobs == 0 ? automaticJobs : Jobs, 1, Math.Max(1, headerCount));
        }
    }

    private sealed record HeaderParseResult(string HeaderPath, List<ReflectTypeInfo> Types);
    private sealed record ParseWorkerRequest(PrecompileParams Config, List<string> Headers);
    private sealed record ParseWorkerResponse(
        List<HeaderParseResult> Headers, ParseCacheStatistics Cache, List<CppSourceFile> Sources);

    private static ParseBatch ParseHeaders(
        string rootDir, IReadOnlyList<string> headers, PrecompileParams config, ParseExecutionSettings execution,
        SharedParseInputs? inputs = null)
    {
        if (headers.Count == 0)
            return new ParseBatch(new Dictionary<string, List<ReflectTypeInfo>>(PathComparer), new(), []);

        if (execution.Mode == "processes")
            return ParseHeadersInProcesses(headers, config, execution.ResolveJobs(headers.Count));

        var cache = new HeaderParseCache(config, inputs);
        var results = new List<ReflectTypeInfo>[headers.Count];
        Parallel.For(0, headers.Count, new ParallelOptions { MaxDegreeOfParallelism = execution.ResolveJobs(headers.Count) },
            index => results[index] = ParseSingleHeader(rootDir, headers[index], cache));
        return new ParseBatch(Enumerable.Range(0, headers.Count).ToDictionary(index => headers[index], index => results[index], PathComparer),
            cache.Statistics, cache.Sources);
    }

    private static List<ReflectTypeInfo> ParseSingleHeader(string rootDir, string header, HeaderParseCache cache)
    {
        try
        {
            // Each task owns its options, native index, AST and materialized
            // models; no lazy CppAst traversal escapes the worker.
            return cache.GetOrParse(rootDir, header);
        }
        catch (Exception ex)
        {
            throw new InvalidOperationException($"Failed to parse '{header}': {ex.Message}", ex);
        }
    }

    private static int RunParseWorker(string requestPath, string responsePath)
    {
        try
        {
            var request = JsonSerializer.Deserialize<ParseWorkerRequest>(File.ReadAllText(requestPath))
                ?? throw new InvalidOperationException("Empty parser worker request.");
            var rootDir = Path.GetFullPath(request.Config.RootDir);
            var cache = new HeaderParseCache(request.Config);
            var results = new List<HeaderParseResult>();
            foreach (var header in request.Headers)
            {
                Console.WriteLine($"[MetaParser] Worker parsing {header}");
                results.Add(new HeaderParseResult(header, ParseSingleHeader(rootDir, header, cache)));
            }
            VerifyCachedSourceFiles(cache.Sources);
            File.WriteAllText(responsePath, JsonSerializer.Serialize(new ParseWorkerResponse(results, cache.Statistics, cache.Sources)));
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[MetaParser] Worker failed: {ex}");
            return 5;
        }
    }

    private static ParseBatch ParseHeadersInProcesses(
        IReadOnlyList<string> headers, PrecompileParams config, int jobs)
    {
        var temporaryDirectory = Path.Combine(Path.GetTempPath(), $"Nullus.MetaParser.{Guid.NewGuid():N}");
        Directory.CreateDirectory(temporaryDirectory);
        using var cancellation = new CancellationTokenSource();
        try
        {
            // One long-lived process per batch, never one process per header.
            var requests = Enumerable.Range(0, jobs).Select(workerIndex =>
            {
                var batch = headers.Where((_, index) => index % jobs == workerIndex).ToList();
                var requestPath = Path.Combine(temporaryDirectory, $"{workerIndex}.request.json");
                var responsePath = Path.Combine(temporaryDirectory, $"{workerIndex}.response.json");
                File.WriteAllText(requestPath, JsonSerializer.Serialize(new ParseWorkerRequest(config, batch)));
                return (Request: requestPath, Response: responsePath, Headers: batch);
            }).ToArray();
            var workers = requests.Select(request =>
                RunParseWorkerProcessAsync(request.Request, request.Response, request.Headers, cancellation)).ToArray();
            var results = Task.WhenAll(workers).GetAwaiter().GetResult();
            return new ParseBatch(results.SelectMany(static batch => batch.Headers)
                    .ToDictionary(static result => result.HeaderPath, static result => result.Types, PathComparer),
                new ParseCacheStatistics(results.Sum(batch => batch.Cache.Hits), results.Sum(batch => batch.Cache.Misses),
                    results.Sum(batch => batch.Cache.SharedHits), results.First().Cache.TotalJobs),
                results.SelectMany(batch => batch.Sources).ToList());
        }
        finally
        {
            Directory.Delete(temporaryDirectory, recursive: true);
        }
    }

    private static async Task<ParseWorkerResponse> RunParseWorkerProcessAsync(
        string requestPath, string responsePath, IReadOnlyList<string> headers, CancellationTokenSource cancellation)
    {
        using var process = new Process();
        try
        {
            var executable = Environment.ProcessPath ?? throw new InvalidOperationException("Cannot locate MetaParser host.");
            var startInfo = new ProcessStartInfo(executable)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            // Support both the published apphost and `dotnet MetaParser.dll`.
            if (string.Equals(Path.GetFileNameWithoutExtension(executable), "dotnet", StringComparison.OrdinalIgnoreCase))
                startInfo.ArgumentList.Add(Assembly.GetExecutingAssembly().Location);
            startInfo.ArgumentList.Add("--parse-worker");
            startInfo.ArgumentList.Add(requestPath);
            startInfo.ArgumentList.Add(responsePath);
            process.StartInfo = startInfo;
            cancellation.Token.ThrowIfCancellationRequested();
            process.Start();
            var outputTask = process.StandardOutput.ReadToEndAsync();
            var errorTask = process.StandardError.ReadToEndAsync();
            try
            {
                await process.WaitForExitAsync(cancellation.Token);
            }
            finally
            {
                if (!process.HasExited)
                {
                    try
                    {
                        process.Kill(entireProcessTree: true);
                    }
                    catch (InvalidOperationException) when (process.HasExited)
                    {
                        // The worker may exit between the check and Kill.
                    }
                    await process.WaitForExitAsync();
                }
                await Task.WhenAll(outputTask, errorTask);
            }
            var output = await outputTask;
            var errors = await errorTask;
            if (process.ExitCode != 0)
                throw new InvalidOperationException($"Parser worker exited with code {process.ExitCode}:{Environment.NewLine}{output}{errors}");
            if (!string.IsNullOrEmpty(errors))
                Console.Error.Write(errors);

            var response = JsonSerializer.Deserialize<ParseWorkerResponse>(File.ReadAllText(responsePath));
            var results = response?.Headers;
            if (response?.Cache is null || response.Sources is null || results is null || results.Count != headers.Count
                || results.Where((result, index) => result.Types is null || !PathComparer.Equals(result.HeaderPath, headers[index])).Any())
                throw new InvalidOperationException("Parser worker returned incomplete or mismatched header results.");
            return response;
        }
        catch
        {
            cancellation.Cancel();
            throw;
        }
    }
}
