using System.Diagnostics;

internal static partial class MetaParserTool
{
    internal static int Run(string[] args)
    {
        if (args.Length == 3 && args[0] == "--parse-worker")
            return RunParseWorker(args[1], args[2]);

        if (args.Length > 0 && args[0] == "--batch")
        {
            if (args.Length != 2)
            {
                Console.Error.WriteLine("Usage: MetaParser --batch <batch.json>");
                return 2;
            }
            try { return RunBatch(args[1]); }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Fatal: {ex}");
                return 5;
            }
        }

        if (args.Length != 1)
        {
            Console.Error.WriteLine("Usage: MetaParser <precompile.json>");
            return 2;
        }

        var paramsPath = Path.GetFullPath(args[0]);
        if (!File.Exists(paramsPath))
        {
            Console.Error.WriteLine($"Params file not found: {paramsPath}");
            return 3;
        }

        try
        {
            var config = PrecompileParams.Load(paramsPath);
            if (config is null || !config.IsValid())
            {
                Console.Error.WriteLine("Invalid precompile params file.");
                return 4;
            }
            return Execute(config);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[MetaParser] Fatal: {ex}");
            return 5;
        }
    }

    private static int Execute(PrecompileParams config)
    {
        _ = ParseExecutionSettings.FromConfig(config);
        using var output = new OutputLocks([config.OutputDir]);
        var inputs = new SharedParseInputs();
        var target = ParseTarget(PrepareTarget(config, inputs), inputs);
        using (inputs.Profile.Measure(CacheOperation.FinalVerification))
            VerifyCachedSourceFiles(target.Parsed.Sources);
        GenerateTarget(target);
        inputs.Profile.Write();
        return 0;
    }

    private sealed record PreparedTarget(PrecompileParams Config, ParseExecutionSettings Execution,
        string RootDir, string OutputDir, List<string> SelectedHeaders, List<string> ParseHeaders,
        Dictionary<string, string> RuntimeHeaders, Stopwatch Timer, double ScanMilliseconds);

    private sealed record ParsedTarget(PreparedTarget Plan, ParseBatch Parsed, List<ReflectTypeInfo> Types,
        ReflectionTypeCatalog Catalog, double ParseMilliseconds, double CatalogMilliseconds);

    private static PreparedTarget PrepareTarget(PrecompileParams config, SharedParseInputs inputs)
    {
        var execution = ParseExecutionSettings.FromConfig(config);
        var timer = Stopwatch.StartNew();
        var rootDir = Path.GetFullPath(config.RootDir);
        var outputDir = Path.GetFullPath(config.OutputDir);
        Directory.CreateDirectory(outputDir);

        var headers = config.Headers
            .Select(Path.GetFullPath)
            .Distinct(PathComparer)
            .ToList();

        var runtimeHeaders = inputs.RuntimeHeaders(rootDir);

        EnsureGeneratedHeaderStubs(
            headers.Concat(runtimeHeaders.Keys).Distinct(PathComparer),
            rootDir,
            outputDir,
            MetaParserGeneratorRegistry.All);

        var selectedHeaders = headers
            .Where(header => File.Exists(header) && ShouldParseHeader(header, MetaParserGeneratorRegistry.All))
            .ToList();
        var parseHeaders = selectedHeaders.Concat(runtimeHeaders.Keys).Distinct(PathComparer).ToList();
        foreach (var header in selectedHeaders)
        {
            var headerText = File.ReadAllText(header);
            var routes = DescribeHeaderParseRoutes(headerText);
            Console.WriteLine($"[MetaParser] Parsing {header} [routes: {routes}]");
        }

        return new PreparedTarget(config, execution, rootDir, outputDir, selectedHeaders, parseHeaders,
            runtimeHeaders, timer, timer.Elapsed.TotalMilliseconds);
    }

    private static ParsedTarget ParseTarget(PreparedTarget target, SharedParseInputs inputs)
    {
        var timer = Stopwatch.StartNew();
        var parsed = ParseHeaders(target.RootDir, target.ParseHeaders, target.Config, target.Execution, inputs);
        var parsedHeaders = parsed.Types;
        var parseMilliseconds = timer.Elapsed.TotalMilliseconds;
        var orderedTypes = MergeReflectTypes(target.SelectedHeaders.SelectMany(header => parsedHeaders[header]))
            .OrderBy(static type => type.HeaderPath, StringComparer.Ordinal)
            .ThenBy(static type => type.QualifiedName, StringComparer.Ordinal)
            .ToList();

        var reflectionTypeCatalog = BuildReflectionTypeCatalog(target.RootDir, orderedTypes, target.RuntimeHeaders, parsedHeaders);
        ValidateReflectTypes(orderedTypes, reflectionTypeCatalog);
        var catalogMilliseconds = timer.Elapsed.TotalMilliseconds - parseMilliseconds;
        return new ParsedTarget(target, parsed, orderedTypes, reflectionTypeCatalog, parseMilliseconds, catalogMilliseconds);
    }

    private static void GenerateTarget(ParsedTarget target)
    {
        var timer = Stopwatch.StartNew();
        var config = target.Plan.Config;
        var outputDir = target.Plan.OutputDir;
        foreach (var generator in MetaParserGeneratorRegistry.All)
            generator.Generate(config, target.Types, target.Catalog, outputDir);

        WriteScriptApiManifest(target.Plan.RootDir, outputDir, config.TargetName, target.Types);

        var generationMilliseconds = timer.Elapsed.TotalMilliseconds;
        Console.WriteLine(FormattableString.Invariant(
            $"[MetaParser] Mode={target.Plan.Execution.Mode}, Jobs={target.Plan.Execution.ResolveJobs(target.Plan.ParseHeaders.Count)}, UniqueHeaders={target.Plan.ParseHeaders.Count}, Scan={target.Plan.ScanMilliseconds:F0}ms, Parse={target.ParseMilliseconds:F0}ms, Catalog={target.CatalogMilliseconds:F0}ms, Generate={generationMilliseconds:F0}ms, Total={target.Plan.Timer.Elapsed.TotalMilliseconds:F0}ms"));
        var parsed = target.Parsed;
        Console.WriteLine($"[MetaParser] CacheHits={parsed.Cache.Hits}, CacheMisses={parsed.Cache.Misses}, SharedCacheHits={parsed.Cache.SharedHits}, TotalParseJobs={parsed.Cache.TotalJobs}");
        Console.WriteLine($"[MetaParser] Target={config.TargetName}, Types={target.Types.Count}, Output={outputDir}");
    }
}
