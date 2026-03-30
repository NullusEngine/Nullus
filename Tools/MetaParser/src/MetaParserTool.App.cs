using System.Text;

internal static partial class MetaParserTool
{
    internal static int Run(string[] args)
    {
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

        var config = PrecompileParams.Load(paramsPath);
        if (config is null || !config.IsValid())
        {
            Console.Error.WriteLine("Invalid precompile params file.");
            return 4;
        }

        try
        {
            return Execute(config);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[MetaParser] Fatal: {ex.Message}");
            return 5;
        }
    }

    private static int Execute(PrecompileParams config)
    {
        var rootDir = Path.GetFullPath(config.RootDir);
        var outputDir = Path.GetFullPath(config.OutputDir);
        Directory.CreateDirectory(outputDir);

        var headers = config.Headers
            .Select(Path.GetFullPath)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

        EnsureGeneratedHeaderStubs(headers, rootDir, outputDir, MetaParserGeneratorRegistry.All);

        var discoveredTypes = new List<ReflectTypeInfo>();
        foreach (var header in headers)
        {
            if (!File.Exists(header) || !ShouldParseHeader(header, MetaParserGeneratorRegistry.All))
                continue;

            var headerText = File.ReadAllText(header);
            var routes = DescribeHeaderParseRoutes(headerText);
            Console.WriteLine($"[MetaParser] Parsing {header} [routes: {routes}]");
            discoveredTypes.AddRange(ParseHeader(rootDir, header, config));
        }

        var orderedTypes = MergeReflectTypes(discoveredTypes)
            .OrderBy(static type => type.HeaderPath, StringComparer.Ordinal)
            .ThenBy(static type => type.QualifiedName, StringComparer.Ordinal)
            .ToList();

        ValidateReflectTypes(rootDir, orderedTypes);
        foreach (var generator in MetaParserGeneratorRegistry.All)
            generator.Generate(config, orderedTypes, outputDir);

        Console.WriteLine($"[MetaParser] Target={config.TargetName}, Types={orderedTypes.Count}, Output={outputDir}");
        return 0;
    }
}
