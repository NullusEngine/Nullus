using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json;

internal static partial class MetaParserTool
{
    private sealed class SharedParseInputs
    {
        internal sealed class SearchScope
        {
            public SearchScope(CacheLookupProfile profile) => Lookups = new(profile);
            public CacheLookupMemo Lookups { get; }
            public ConcurrentDictionary<string, Lazy<string>> Trees { get; } = new(PathComparer);
            public ConcurrentDictionary<string, Lazy<string>> Ancestors { get; } = new(PathComparer);
        }

        public ConcurrentDictionary<string, Lazy<string>> Hashes { get; } = new(PathComparer);
        public ConcurrentDictionary<string, string> Texts { get; } = new(PathComparer);
        public ConcurrentDictionary<string, Lazy<SourceVisibilityAnalysis>> SourceAnalyses { get; } = new(PathComparer);
        public CacheLookupProfile Profile { get; } = new();
        private readonly ConcurrentDictionary<string, SearchScope> _searchScopes = new(PathComparer);
        private readonly ConcurrentDictionary<string, Lazy<Dictionary<string, string>>> _runtimeHeaders = new(PathComparer);

        public SearchScope ForCacheDirectory(string directory) => _searchScopes.GetOrAdd(directory, _ => new(Profile));
        public Dictionary<string, string> RuntimeHeaders(string root)
            => _runtimeHeaders.GetOrAdd(root, root => new(() => ReadRuntimeReflectionHeaders(root))).Value;
        public int DirectoryScans => _searchScopes.Values.Sum(scope => scope.Trees.Count + scope.Ancestors.Count);
    }

    private sealed class BatchManifest
    {
        public List<string> Configs { get; set; } = [];
        public int TargetJobs { get; set; }
    }

    private sealed class OutputLocks : IDisposable
    {
        private readonly List<FileStream> _locks = [];

        public OutputLocks(IEnumerable<string> outputs)
        {
            try
            {
                foreach (var output in outputs.Select(Path.GetFullPath).Distinct(PathComparer).Order(PathComparer))
                {
                    Directory.CreateDirectory(output);
                    var timer = Stopwatch.StartNew();
                    while (true)
                    {
                        try
                        {
                            _locks.Add(new FileStream(Path.TrimEndingDirectorySeparator(output) + ".metaparser.lock",
                                FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None));
                            break;
                        }
                        catch (IOException ex) when ((ex.HResult & 0xffff) is 11 or 32 or 33
                            && timer.Elapsed < TimeSpan.FromMinutes(10))
                        { Thread.Sleep(25); }
                    }
                }
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        public void Dispose()
        {
            foreach (var stream in _locks)
                stream.Dispose();
            _locks.Clear();
        }
    }

    private static int RunBatch(string path)
    {
        path = Path.GetFullPath(path);
        if (!File.Exists(path))
        {
            Console.Error.WriteLine($"Batch manifest not found: {path}");
            return 3;
        }
        var manifest = JsonSerializer.Deserialize<BatchManifest>(File.ReadAllText(path),
            new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        if (manifest is null || manifest.Configs.Count == 0 || manifest.TargetJobs < 0)
            throw new ArgumentException("A batch requires Configs and a nonnegative TargetJobs value.");
        var configPaths = manifest.Configs.Select(config => Path.GetFullPath(config, Path.GetDirectoryName(path)!))
            .Distinct(PathComparer).ToList();
        var configs = configPaths.Select(configPath => PrecompileParams.Load(configPath)
            ?? throw new ArgumentException($"Invalid precompile configuration: {configPath}")).ToList();
        if (configs.Any(config => !config.IsValid()))
            throw new ArgumentException("Invalid precompile configuration in batch.");
        foreach (var config in configs)
            _ = ParseExecutionSettings.FromConfig(config);
        for (var index = 0; index < configs.Count; ++index)
        {
            for (var other = index + 1; other < configs.Count; ++other)
            {
                var left = Path.GetFullPath(configs[index].OutputDir);
                var right = Path.GetFullPath(configs[other].OutputDir);
                if (PathComparer.Equals(left, right) || IsPathInside(left, right) || IsPathInside(right, left))
                    throw new ArgumentException($"Batch output directories overlap: {left}, {right}");
                if (PathComparer.Equals(Path.GetFullPath(configs[index].RootDir), Path.GetFullPath(configs[other].RootDir))
                    && configs[index].TargetName == configs[other].TargetName)
                    throw new ArgumentException($"Duplicate batch target: {configs[index].TargetName}");
            }
        }

        var targetJobs = Math.Clamp(manifest.TargetJobs == 0 ? Math.Clamp(Environment.ProcessorCount / 2, 1, 4)
            : manifest.TargetJobs, 1, configs.Count);
        var timer = Stopwatch.StartNew();
        using var outputs = new OutputLocks(configs.Select(config => config.OutputDir));
        var inputs = new SharedParseInputs();
        // Finish every stub before capturing shared input/namespace snapshots.
        var plans = configs.Select(config => PrepareTarget(config, inputs)).ToList();
        var results = new ParsedTarget[plans.Count];
        Parallel.For(0, plans.Count, new ParallelOptions { MaxDegreeOfParallelism = targetJobs }, index =>
        {
            try { results[index] = ParseTarget(plans[index], inputs); }
            catch (Exception ex) { throw new InvalidOperationException($"Batch target {plans[index].Config.TargetName}: {ex.Message}", ex); }
        });
        // Source validation is performed once for the union of target inputs.
        // Generation cannot change a sibling's stub while parsing is ongoing.
        using (inputs.Profile.Measure(CacheOperation.FinalVerification))
            VerifyCachedSourceFiles(results.SelectMany(result => result.Parsed.Sources));
        Parallel.ForEach(results, new ParallelOptions { MaxDegreeOfParallelism = targetJobs }, GenerateTarget);
        Console.WriteLine(FormattableString.Invariant(
            $"[MetaParser] BatchTargets={configs.Count}, TargetJobs={targetJobs}, FileHashes={inputs.Hashes.Count}, DirectoryScans={inputs.DirectoryScans}, Total={timer.Elapsed.TotalMilliseconds:F0}ms"));
        inputs.Profile.Write();
        return 0;
    }
}
