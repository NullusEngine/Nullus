using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using CppAst;

internal static partial class MetaParserTool
{
    private sealed record ParseCacheStatistics(int Hits = 0, int Misses = 0, int SharedHits = 0, int TotalJobs = 0);
    private sealed record ParseBatch(
        Dictionary<string, List<ReflectTypeInfo>> Types, ParseCacheStatistics Cache, List<CppSourceFile> Sources);
    private sealed record ParseCacheEnvelope(string Checksum, string Payload);
    private sealed record ParseSearchInput(string Path, string Hash);
    private sealed record ParseCacheEntry(
        int Version, string Context, string Header, List<string> Defines, List<string> VisibilityDefines,
        List<string> SearchRoots, string SearchFingerprint, List<CppSourceFile> Sources, List<ReflectTypeInfo> Types);

    private sealed class HeaderParseCache
    {
        private const int Version = 1;
        private static readonly Lazy<string> NativeIdentity = new(() =>
        {
            var directories = new[] { AppContext.BaseDirectory }
                .Concat(new[] { AppContext.GetData("NATIVE_DLL_SEARCH_DIRECTORIES") as string,
                    Environment.GetEnvironmentVariable("PATH"), Environment.GetEnvironmentVariable("LD_LIBRARY_PATH"),
                    Environment.GetEnvironmentVariable("DYLD_LIBRARY_PATH") }
                    .Where(value => value is not null).SelectMany(value => value!.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)))
                .Distinct(PathComparer).ToList();
            var names = OperatingSystem.IsWindows() ? new[] { "libclang.dll", "libClangSharp.dll" }
                : OperatingSystem.IsMacOS() ? new[] { "libclang.dylib", "libClangSharp.dylib" }
                : new[] { "libclang.so", "libClangSharp.so" };
            var files = names.Select(name => directories.Select(directory => Path.Combine(directory, name)).FirstOrDefault(File.Exists))
                .Select(path => path is null ? "unresolved" : Path.GetFullPath(path) + ":" + HashFile(path)).ToArray();
            return JsonSerializer.Serialize(new { Version = CppParser.ClangVersion, Files = files });
        });
        private readonly PrecompileParams _config;
        private readonly string _directory;
        private readonly string _directoryPrefix;
        private readonly string _exactContext = string.Empty;
        private readonly string _sharedContext = string.Empty;
        private readonly HashSet<string> _visibilityDefines;
        private readonly List<string> _searchRoots;
        private readonly ConcurrentDictionary<string, Lazy<string>> _hashes = new(PathComparer);
        private readonly ConcurrentDictionary<string, Lazy<string>> _trees = new(PathComparer);
        private readonly ConcurrentDictionary<string, Lazy<string>> _ancestors = new(PathComparer);
        private readonly ConcurrentDictionary<string, string> _texts = new(PathComparer);
        private readonly ConcurrentDictionary<string, Lazy<SourceVisibilityAnalysis>> _sourceAnalyses = new(PathComparer);
        private readonly CacheLookupMemo _lookups = new(new CacheLookupProfile());
        private readonly ConcurrentDictionary<string, CppSourceFile> _used = new(PathComparer);
        private readonly int _totalJobs;
        private bool _enabled;
        private bool _canLock;
        private int _hits, _misses, _sharedHits;

        public ParseCacheStatistics Statistics => new(_hits, _misses, _sharedHits, _totalJobs);
        public List<CppSourceFile> Sources => _used.Values.ToList();

        public HeaderParseCache(PrecompileParams config, SharedParseInputs? shared = null)
        {
            _config = config;
            _directory = Path.GetFullPath(Environment.GetEnvironmentVariable("NLS_META_PARSER_CACHE_DIR")
                ?? (string.IsNullOrWhiteSpace(config.CacheDirectory)
                    ? Path.Combine(config.RootDir, "Library", "MetaParserCache") : config.CacheDirectory));
            _directoryPrefix = Path.TrimEndingDirectorySeparator(_directory) + Path.DirectorySeparatorChar;
            if (shared is not null)
            {
                _hashes = shared.Hashes;
                _texts = shared.Texts;
                _sourceAnalyses = shared.SourceAnalyses;
                var search = shared.ForCacheDirectory(_directory);
                _trees = search.Trees;
                _ancestors = search.Ancestors;
                _lookups = search.Lookups;
            }
            _visibilityDefines = new HashSet<string>(config.VisibilityDefines, StringComparer.Ordinal);
            if (_visibilityDefines.Any(name => !Regex.IsMatch(name, @"^[A-Za-z_]\w*$", RegexOptions.CultureInvariant)))
                throw new ArgumentException("VisibilityDefines must contain macro names without values.");
            if (config.CompilerOptions.Any(argument => argument.StartsWith("-D", StringComparison.Ordinal)
                || argument.StartsWith("-U", StringComparison.Ordinal) || argument.StartsWith("/D", StringComparison.Ordinal)
                || argument.StartsWith("/U", StringComparison.Ordinal)))
                _visibilityDefines.Clear();
            _totalJobs = config.TotalParseJobs;
            var environmentJobs = Environment.GetEnvironmentVariable("NLS_META_PARSER_TOTAL_JOBS");
            if (environmentJobs is not null && !int.TryParse(environmentJobs, out _totalJobs))
                throw new ArgumentException("NLS_META_PARSER_TOTAL_JOBS must be a nonnegative integer.");
            if (_totalJobs < 0)
                throw new ArgumentException("TotalParseJobs must be a nonnegative integer.");
            if (_totalJobs == 0)
                _totalJobs = Math.Clamp(Environment.ProcessorCount / 2, 1, 8);
            _enabled = config.UseCache;
            var environmentCache = Environment.GetEnvironmentVariable("NLS_META_PARSER_CACHE");
            if (environmentCache is not null)
            {
                _enabled = environmentCache.Trim().ToLowerInvariant() switch
                {
                    "1" or "true" or "on" => true,
                    "0" or "false" or "off" => false,
                    _ => throw new ArgumentException("NLS_META_PARSER_CACHE must be on or off.")
                };
            }
            // PCH, modules and response files have additional inputs outside an
            // inclusion graph. Preserve their behavior with uncached parsing.
            if (config.CompilerOptions.Any(argument => argument.StartsWith('@')
                || Regex.IsMatch(argument, @"(?:include-pch|fmodules|fmodule-|ivfsoverlay|vfsoverlay|remap-file|ftime-trace)|^(?:-I|-F|-iquote|-isystem|-isysroot|--sysroot|-imsvc|-idirafter|-iframework|-iprefix|-iwithprefix|-Xclang|--config|-resource-dir|-B)", RegexOptions.CultureInvariant)))
                _enabled = false;
            _searchRoots = config.IncludeDirs.Concat(config.SystemIncludeDirs)
                .Concat(new[] { "CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "INCLUDE" }
                    .SelectMany(name => (Environment.GetEnvironmentVariable(name) ?? string.Empty)
                        .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)))
                .Where(static path => !string.IsNullOrWhiteSpace(path)).Select(Path.GetFullPath).Distinct(PathComparer).ToList();
            if (!string.IsNullOrEmpty(config.ResourceDir))
                _searchRoots.Add(Path.GetFullPath(config.ResourceDir));
            // Default system searches can probe a header that has never been
            // included before. Witness the SDK namespace as well as read files.
            foreach (var root in DiscoverSystemSearchRoots(config))
                if (!string.IsNullOrWhiteSpace(root) && !PathComparer.Equals(Path.GetFullPath(root), Path.GetPathRoot(Path.GetFullPath(root))))
                    _searchRoots.Add(Path.GetFullPath(root));
            if (_searchRoots.Any(root => PathComparer.Equals(root, _directory) || IsPathInside(root, _directory)))
                throw new ArgumentException("CacheDirectory must not contain source/include directories.");
            try
            {
                Directory.CreateDirectory(Path.Combine(_directory, "slots"));
                _canLock = true;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                _enabled = false;
                Console.Error.WriteLine($"[MetaParser] Cache directory unavailable; parsing without shared cache/budget: {ex.Message}");
            }
            if (_enabled)
            {
                _exactContext = ComputeContext(config.Defines);
                _sharedContext = ComputeContext(config.Defines.Where(define => !_visibilityDefines.Contains(DefineName(define))));
            }
        }

        public List<ReflectTypeInfo> GetOrParse(string rootDir, string header)
        {
            if (!_enabled)
                return ParseUncached(rootDir, header, collectSources: false).Types;
            var headerKey = Hash(Encoding.UTF8.GetBytes(Path.GetFullPath(header)));
            var exactPath = Path.Combine(_directory, _exactContext, headerKey + ".json");
            var sharedPath = Path.Combine(_directory, _sharedContext, headerKey + ".shared.json");
            if (TryLoadPreferred(exactPath, sharedPath, header, out var cached))
                return cached!;
            var lockPath = (_visibilityDefines.Count == 0 ? exactPath : sharedPath) + ".lock";
            FileStream? entryLock;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(lockPath)!);
                entryLock = AcquireLock(lockPath);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                Console.Error.WriteLine($"[MetaParser] Cache lock unavailable for {header}: {ex.Message}");
                return ParseUncached(rootDir, header, collectSources: false).Types;
            }
            using (entryLock)
            {
                // The producer may have completed while this process waited.
                if (TryLoadPreferred(exactPath, sharedPath, header, out cached))
                    return cached!;
                // Capture the explicit search namespace before libclang reads
                // inputs, including absent files that __has_include may probe.
                try
                {
                    _ = SearchFingerprint(MinimalRoots(_searchRoots.Append(Path.GetDirectoryName(header)!)));
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                    Console.Error.WriteLine($"[MetaParser] Input search tree cannot be cached for {header}: {ex.Message}");
                    return ParseUncached(rootDir, header, collectSources: false).Types;
                }
                var parsed = ParseUncached(rootDir, header, collectSources: true);
                if (parsed.Sources.Count == 0 || !parsed.Sources.Any(source => PathComparer.Equals(source.Path, header)))
                    return parsed.Types;
                var roots = MinimalRoots(_searchRoots.Append(Path.GetDirectoryName(header)!)
                    .Concat(parsed.Sources.Select(source => Path.GetDirectoryName(source.Path)!)));
                try
                {
                    var entry = new ParseCacheEntry(Version, _exactContext, header, _config.Defines, _config.VisibilityDefines,
                        roots, SearchFingerprint(roots), parsed.Sources, parsed.Types);
                    if (SourcesMatch(entry.Sources) && !UsesVolatileMacros(entry.Sources))
                    {
                        Store(exactPath, entry);
                        _lookups.Invalidate(exactPath);
                        if (_visibilityDefines.Count > 0 && VisibilityOnly(entry.Sources, _visibilityDefines))
                        {
                            Store(sharedPath, entry with { Context = _sharedContext });
                            _lookups.Invalidate(sharedPath);
                        }
                    }
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                    Console.Error.WriteLine($"[MetaParser] Could not publish cache entry for {header}: {ex.Message}");
                }
                return parsed.Types;
            }
        }

        private HeaderParseData ParseUncached(string rootDir, string header, bool collectSources)
        {
            using var slot = AcquireSlot();
            var sources = collectSources ? new List<CppSourceFile>() : null;
            var types = ParseHeader(rootDir, header, _config, sources).ToList();
            Interlocked.Increment(ref _misses);
            if (sources is not null)
                Remember(sources);
            return new HeaderParseData(types, sources ?? []);
        }

        private sealed record HeaderParseData(List<ReflectTypeInfo> Types, List<CppSourceFile> Sources);

        private FileStream? AcquireSlot()
        {
            if (!_canLock)
                return null;
            var timer = Stopwatch.StartNew();
            while (true)
            {
                for (var index = 0; index < _totalJobs; ++index)
                {
                    try
                    {
                        return new FileStream(Path.Combine(_directory, "slots", index + ".lock"),
                            FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
                    }
                    catch (IOException ex) when (IsSharingViolation(ex)) { }
                    catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                    {
                        _canLock = false;
                        Console.Error.WriteLine($"[MetaParser] Shared parsing budget unavailable: {ex.Message}");
                        return null;
                    }
                }
                if (timer.Elapsed > TimeSpan.FromMinutes(10))
                    throw new TimeoutException("Timed out waiting for a shared MetaParser parsing slot.");
                Thread.Sleep(25);
            }
        }

        private static FileStream AcquireLock(string path)
        {
            var timer = Stopwatch.StartNew();
            while (true)
            {
                try { return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
                catch (IOException ex) when (IsSharingViolation(ex) && timer.Elapsed < TimeSpan.FromMinutes(10))
                { Thread.Sleep(25); }
            }
        }

        private static bool IsSharingViolation(IOException exception)
            => (exception.HResult & 0xffff) is 11 or 32 or 33;

        private bool TryLoadPreferred(string exactPath, string sharedPath, string header, out List<ReflectTypeInfo>? types)
        {
            types = null;
            return (_visibilityDefines.Count > 0 && TryLoad(sharedPath, header, shared: true, out types))
                || TryLoad(exactPath, header, shared: false, out types);
        }

        private bool TryLoad(string path, string header, bool shared, out List<ReflectTypeInfo>? types)
        {
            types = null;
            try
            {
                var loaded = _lookups.Read(path);
                if (loaded is null)
                    return false;
                var entry = loaded.Entry;
                if (entry.Version != Version || !PathComparer.Equals(entry.Header, header)
                    || entry.Context != (shared ? _sharedContext : _exactContext)
                    || !_lookups.CommonValidation.GetOrAdd(loaded.Checksum, _ => new Lazy<bool>(() => ValidateCommon(entry))).Value)
                    return false;
                if (shared)
                {
                    var validationKey = loaded.Checksum + ":" + _exactContext;
                    if (!_lookups.ContextValidation.GetOrAdd(validationKey,
                            _ => new Lazy<bool>(() => ValidateVisibilityContext(entry))).Value)
                        return false;
                    Interlocked.Increment(ref _sharedHits);
                }
                Remember(entry.Sources);
                Interlocked.Increment(ref _hits);
                types = entry.Types;
                return true;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException or NullReferenceException)
            { return false; }
        }

        private bool ValidateCommon(ParseCacheEntry entry)
        {
            using var measurement = _lookups.Profile.Measure(CacheOperation.CommonValidation);
            return entry.Sources is not null && entry.Sources.Count > 0 && entry.Types is not null
                && entry.SearchRoots is not null && entry.Defines is not null && entry.VisibilityDefines is not null
                && entry.Types.All(type => type is not null && type.Bases is not null && type.Fields is not null && type.Methods is not null
                    && PathComparer.Equals(type.SourceFilePath, entry.Header))
                && entry.SearchFingerprint == SearchFingerprint(entry.SearchRoots) && SourcesMatch(entry.Sources);
        }

        private bool ValidateVisibilityContext(ParseCacheEntry entry)
        {
            using var measurement = _lookups.Profile.Measure(CacheOperation.ContextValidation);
            var previous = EffectiveDefines(entry.Defines);
            var current = EffectiveDefines(_config.Defines);
            var changed = previous.Keys.Union(current.Keys)
                .Where(name => previous.GetValueOrDefault(name) != current.GetValueOrDefault(name)).ToList();
            var declared = new HashSet<string>(entry.VisibilityDefines.Concat(_visibilityDefines), StringComparer.Ordinal);
            return changed.All(declared.Contains) && VisibilityOnly(entry.Sources, changed);
        }

        private void Remember(IEnumerable<CppSourceFile> sources)
        {
            foreach (var source in sources)
            {
                _used.AddOrUpdate(source.Path, source, (_, previous) =>
                {
                    if (previous.ContentHash != source.ContentHash && !IsGeneratedHeader(source.Path))
                        throw new IOException($"Source changed during parsing: {source.Path}");
                    return source;
                });
            }
        }

        private bool SourcesMatch(IEnumerable<CppSourceFile> sources)
            => sources.All(source => source is not null
                && _hashes.GetOrAdd(source.Path, path => new Lazy<string>(() => HashInputOrMissing(path))).Value == source.ContentHash);

        private static string HashInputOrMissing(string path)
        {
            try { return HashFile(path); }
            catch (FileNotFoundException) { return string.Empty; }
            catch (DirectoryNotFoundException) { return string.Empty; }
        }

        private string SourceText(string path)
            => _texts.GetOrAdd(path, static path => StripCommentsAndStrings(File.ReadAllText(path)));

        private bool UsesVolatileMacros(IEnumerable<CppSourceFile> sources)
            => sources.Any(source => Regex.IsMatch(SourceText(source.Path), @"\b__(?:DATE|TIME|TIMESTAMP)__\b", RegexOptions.CultureInvariant));

        private bool VisibilityOnly(IEnumerable<CppSourceFile> sources, IEnumerable<string> names)
        {
            var macros = names.ToList();
            if (macros.Count == 0)
                return true;
            var analyses = sources.Select(source => _sourceAnalyses.GetOrAdd(source.Path,
                path => new Lazy<SourceVisibilityAnalysis>(() => AnalyzeSource(path))).Value).ToList();
            var aliases = new HashSet<string>(StringComparer.Ordinal);
            foreach (var alias in new[] { "DLLEXPORT", "DLLIMPORT" })
            {
                var definitions = analyses.SelectMany(analysis => alias == "DLLEXPORT"
                    ? analysis.ExportDefinitions : analysis.ImportDefinitions).ToList();
                definitions.AddRange(_config.Defines.Where(define => DefineName(define) == alias)
                    .Select(define => define.Contains('=') ? define[(define.IndexOf('=') + 1)..] : "1"));
                if (definitions.Count > 0 && definitions.All(body => PureVisibility(body, [])))
                    aliases.Add(alias);
            }
            var aliasKey = (aliases.Contains("DLLEXPORT") ? "E" : "-") + (aliases.Contains("DLLIMPORT") ? "I" : "-");
            foreach (var analysis in analyses)
            {
                // Constructed export-macro names cannot be proven equivalent by
                // the simple visibility-block check. Keep them in exact contexts.
                if (analysis.UnsupportedConstruction)
                    return false;
                foreach (var macro in macros)
                {
                    if (!analysis.MacroChecks.GetOrAdd(macro + ":" + aliasKey,
                            _ => new Lazy<bool>(() => CheckSourceMacro(analysis, macro, aliases))).Value)
                        return false;
                }
            }
            return true;
        }

        private SourceVisibilityAnalysis AnalyzeSource(string path)
        {
            using var measurement = _lookups.Profile.Measure(CacheOperation.SourceAnalysis);
            return new SourceVisibilityAnalysis(SourceText(path));
        }

        private bool CheckSourceMacro(SourceVisibilityAnalysis source, string macro, HashSet<string> aliases)
        {
            using var measurement = _lookups.Profile.Measure(CacheOperation.MacroCheck);
            if (!source.Text.Contains(macro, StringComparison.Ordinal))
                return true;
            var reference = new Regex($@"\b{Regex.Escape(macro)}\b", RegexOptions.CultureInvariant);
            var condition = new Regex($@"^\s*#\s*(?:ifdef\s+{Regex.Escape(macro)}|if\s+defined\s*(?:\(\s*{Regex.Escape(macro)}\s*\)|\s+{Regex.Escape(macro)}))\s*$", RegexOptions.CultureInvariant);
            for (var index = 0; index < source.Lines.Length; ++index)
            {
                if (!reference.IsMatch(source.Lines[index]))
                    continue;
                if (!condition.IsMatch(source.Lines[index]) || !VisibilityBlock(source.Lines, ref index, aliases))
                    return false;
            }
            return true;
        }

        private static bool VisibilityBlock(string[] lines, ref int index, HashSet<string> aliases)
        {
            string? definedName = null;
            var definitions = 0;
            var branches = 0;
            for (var cursor = index + 1; cursor < lines.Length; ++cursor)
            {
                var line = lines[cursor].Trim();
                if (line.Length == 0)
                    continue;
                if (Regex.IsMatch(line, @"^#\s*else\s*$", RegexOptions.CultureInvariant))
                { ++branches; continue; }
                if (Regex.IsMatch(line, @"^#\s*endif\s*$", RegexOptions.CultureInvariant))
                {
                    if (definitions != 2 || branches != 1)
                        return false;
                    index = cursor;
                    return true;
                }
                var definition = Regex.Match(line, @"^#\s*define\s+(\w+)\s*(.*)$", RegexOptions.CultureInvariant);
                if (!definition.Success || !PureVisibility(definition.Groups[2].Value, aliases))
                    return false;
                var name = definition.Groups[1].Value;
                if (definedName is not null && name != definedName)
                    return false;
                definedName = name;
                ++definitions;
            }
            return false;
        }

        private static bool PureVisibility(string expression, IEnumerable<string> aliases)
        {
            var value = Regex.Replace(expression.Replace("\"", string.Empty), @"\s", string.Empty, RegexOptions.CultureInvariant);
            return value.Length == 0 || aliases.Contains(value)
                || value is "__declspec(dllexport)" or "__declspec(dllimport)"
                    or "__attribute__((visibility()))" or "__attribute__((__visibility__()))";
        }

        private string ComputeContext(IEnumerable<string> defines)
        {
            var simpleArguments = _config.CompilerOptions.All(argument =>
                Regex.IsMatch(argument, @"^-(?:std=|W|stdlib=|fms-)", RegexOptions.CultureInvariant)
                || (argument.StartsWith("--gcc-toolchain=", StringComparison.Ordinal)
                    && Path.IsPathFullyQualified(argument["--gcc-toolchain=".Length..])));
            var environmentNames = new[] { "PATH", "CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "INCLUDE", "SDKROOT",
                "MACOSX_DEPLOYMENT_TARGET", "VCToolsInstallDir", "VCINSTALLDIR", "WindowsSdkDir", "WindowsSDKVersion" };
            var context = new
            {
                Version,
                Tool = typeof(MetaParserTool).Assembly.ManifestModule.ModuleVersionId,
                CppAst = typeof(CppParser).Assembly.ManifestModule.ModuleVersionId,
                Native = NativeIdentity.Value,
                OS = RuntimeInformation.OSDescription,
                Architecture = RuntimeInformation.ProcessArchitecture,
                Root = Path.GetFullPath(_config.RootDir),
                _config.CompilerPath, _config.CompilerId, _config.CompilerTarget, _config.ResourceDir, _config.Sysroot,
                Includes = _config.IncludeDirs.Where(path => !string.IsNullOrWhiteSpace(path)).Select(Path.GetFullPath).ToArray(),
                SystemIncludes = _config.SystemIncludeDirs.Where(path => !string.IsNullOrWhiteSpace(path)).Select(Path.GetFullPath).ToArray(),
                ResolvedSearchRoots = _searchRoots,
                Defines = defines.ToArray(), Options = _config.CompilerOptions,
                WorkingDirectory = simpleArguments && (string.IsNullOrEmpty(_config.ResourceDir) || Path.IsPathFullyQualified(_config.ResourceDir))
                    && (string.IsNullOrEmpty(_config.Sysroot) || Path.IsPathFullyQualified(_config.Sysroot)) ? null : Environment.CurrentDirectory,
                Environment = environmentNames.ToDictionary(name => name, Environment.GetEnvironmentVariable)
            };
            return Hash(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(context)));
        }

        private string SearchFingerprint(IEnumerable<string> roots)
        {
            var directories = MinimalRoots(roots);
            var parentPaths = new HashSet<string>(PathComparer);
            foreach (var root in directories.Where(root => !_searchRoots.Any(search => PathComparer.Equals(root, search) || IsPathInside(root, search))
                         && !PathComparer.Equals(root, Path.GetFullPath(_config.RootDir)) && !IsPathInside(root, _config.RootDir)))
                for (var parent = Directory.GetParent(root); parent is not null; parent = parent.Parent)
                    parentPaths.Add(parent.FullName);
            var values = directories.Select(path => new ParseSearchInput(path, _trees.GetOrAdd(path,
                    path => new Lazy<string>(() => DirectoryFingerprint(path, recursive: true))).Value))
                .Concat(parentPaths.Order(StringComparer.Ordinal).Select(path => new ParseSearchInput(path, _ancestors.GetOrAdd(path,
                    path => new Lazy<string>(() => DirectoryFingerprint(path, recursive: false))).Value)));
            return Hash(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(values)));
        }

        private string DirectoryFingerprint(string root, bool recursive)
        {
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            var pending = new Stack<string>();
            var visited = new HashSet<string>(PathComparer);
            pending.Push(root);
            while (pending.Count > 0)
            {
                var path = pending.Pop();
                if (InsideCache(path))
                    continue;
                if (!Directory.Exists(path))
                { hash.AppendData(Encoding.UTF8.GetBytes(path + "\0missing\n")); continue; }
                var directory = new DirectoryInfo(path);
                var resolved = (directory.Attributes & FileAttributes.ReparsePoint) != 0
                    ? directory.ResolveLinkTarget(returnFinalTarget: true)?.FullName ?? path : path;
                hash.AppendData(Encoding.UTF8.GetBytes(path + "\0" + resolved + "\n"));
                if (!visited.Add(resolved))
                    continue;
                foreach (var entry in directory.EnumerateFileSystemInfos().OrderBy(entry => entry.Name, StringComparer.Ordinal))
                {
                    if (InsideCache(entry.FullName) || IsAtomicTemporaryFile(entry.Name))
                        continue;
                    // Ancestors witness SDK/version directory discovery, not
                    // unrelated logs or per-target parameter files.
                    var isDirectory = (entry.Attributes & FileAttributes.Directory) != 0;
                    if (!recursive && !isDirectory)
                        continue;
                    hash.AppendData(Encoding.UTF8.GetBytes(entry.FullName + (isDirectory ? "/\n" : "\n")));
                    if (recursive && isDirectory)
                        pending.Push(entry.FullName);
                }
            }
            return Convert.ToHexString(hash.GetHashAndReset());
        }

        private bool InsideCache(string path)
            => PathComparer.Equals(path, _directory) || path.StartsWith(_directoryPrefix,
                OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

        private static bool IsAtomicTemporaryFile(string name)
            => name.Length >= 37 && name.EndsWith(".tmp", StringComparison.Ordinal)
                && name[^37] == '.' && Guid.TryParseExact(name.AsSpan(name.Length - 36, 32), "N", out _);

        private static List<string> DiscoverSystemSearchRoots(PrecompileParams config)
        {
            var roots = new List<string>();
            if (!string.IsNullOrWhiteSpace(config.Sysroot))
                roots.Add(config.Sysroot);
            var environmentSdk = Environment.GetEnvironmentVariable("SDKROOT");
            if (!string.IsNullOrWhiteSpace(environmentSdk))
                roots.Add(environmentSdk);
            if (OperatingSystem.IsWindows())
            {
                var compiler = config.CompilerPath.Replace('\\', '/');
                var msvc = Regex.Match(compiler, @"(?i)^(.*?/VC/Tools/MSVC)/", RegexOptions.CultureInvariant);
                if (msvc.Success)
                    roots.Add(msvc.Groups[1].Value);
                var sdk = Environment.GetEnvironmentVariable("WindowsSdkDir");
                try
                {
                    sdk ??= Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows Kits\Installed Roots", "KitsRoot10", null) as string;
                    sdk ??= Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots", "KitsRoot10", null) as string;
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.Security.SecurityException) { }
                if (!string.IsNullOrWhiteSpace(sdk))
                    roots.Add(Path.Combine(sdk, "Include"));
            }
            else if (OperatingSystem.IsLinux())
            {
                roots.Add("/usr/include");
                roots.Add("/usr/local/include");
            }
            return roots;
        }

        private static List<string> MinimalRoots(IEnumerable<string> roots)
        {
            var result = new List<string>();
            foreach (var root in roots.Select(Path.GetFullPath).Distinct(PathComparer).OrderBy(path => path.Length).ThenBy(path => path, StringComparer.Ordinal))
                if (!result.Any(parent => PathComparer.Equals(root, parent) || IsPathInside(root, parent)))
                    result.Add(root);
            return result;
        }

        private static string DefineName(string define)
            => define.Split(['=', '('], 2)[0].Trim();

        private static Dictionary<string, string> EffectiveDefines(IEnumerable<string> defines)
            => defines.GroupBy(DefineName, StringComparer.Ordinal).ToDictionary(group => group.Key, group => group.Last(), StringComparer.Ordinal);

        private static void Store(string path, ParseCacheEntry entry)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var payload = JsonSerializer.Serialize(entry);
            WriteAllTextIfChanged(path, JsonSerializer.Serialize(new ParseCacheEnvelope(Hash(Encoding.UTF8.GetBytes(payload)), payload)));
        }

        private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes));
    }

    private static bool IsGeneratedHeader(string path)
        => path.EndsWith(".generated.h", StringComparison.OrdinalIgnoreCase)
            || Path.GetFileName(path).EndsWith("MetaGenerated.h", StringComparison.OrdinalIgnoreCase);

    private static string HashFile(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    private static void VerifyCachedSourceFiles(IEnumerable<CppSourceFile> sources)
    {
        foreach (var group in sources.GroupBy(source => source.Path, PathComparer))
        {
            // Other targets may legitimately replace a generated stub while
            // this target parses. Its old fingerprint will miss next time.
            if (IsGeneratedHeader(group.Key))
                continue;
            var expected = group.First().ContentHash;
            if (group.Any(source => source.ContentHash != expected) || !File.Exists(group.Key) || HashFile(group.Key) != expected)
                throw new IOException($"Source changed during reflection parsing; rerun generation: {group.Key}");
        }
    }
}
