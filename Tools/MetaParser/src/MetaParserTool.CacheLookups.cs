using System.Collections.Concurrent;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

internal static partial class MetaParserTool
{
    private enum CacheOperation { Read, Checksum, Decode, CommonValidation, ContextValidation, SourceAnalysis, MacroCheck, FinalVerification }

    private sealed class CacheLookupProfile
    {
        private readonly long[] _counts = new long[Enum.GetValues<CacheOperation>().Length];
        private readonly long[] _ticks = new long[Enum.GetValues<CacheOperation>().Length];
        private long _loadReuses;
        public void ReusedLoad() => Interlocked.Increment(ref _loadReuses);
        public Measurement Measure(CacheOperation operation) => new(this, operation);

        public readonly struct Measurement : IDisposable
        {
            private readonly CacheLookupProfile _owner;
            private readonly CacheOperation _operation;
            private readonly long _start;
            public Measurement(CacheLookupProfile owner, CacheOperation operation)
            {
                _owner = owner;
                _operation = operation;
                _start = Stopwatch.GetTimestamp();
                Interlocked.Increment(ref owner._counts[(int)operation]);
            }
            public void Dispose() => Interlocked.Add(ref _owner._ticks[(int)_operation], Stopwatch.GetTimestamp() - _start);
        }

        public void Write()
        {
            Console.WriteLine($"[MetaParser] CacheReads={_counts[(int)CacheOperation.Read]}, CacheLoadReuses={_loadReuses}, CacheDecodes={_counts[(int)CacheOperation.Decode]}, CacheValidations={_counts[(int)CacheOperation.CommonValidation]}, SourceAnalyses={_counts[(int)CacheOperation.SourceAnalysis]}, MacroChecks={_counts[(int)CacheOperation.MacroCheck]}");
            Console.WriteLine("[MetaParser] CacheTime=" + string.Join(", ", Enum.GetValues<CacheOperation>().Select(operation =>
                FormattableString.Invariant($"{operation}:{1000.0 * _ticks[(int)operation] / Stopwatch.Frequency:F1}ms"))));
        }
    }

    private sealed record LoadedParseCacheEntry(string Checksum, ParseCacheEntry Entry);
    private sealed record CacheFileIdentity(string Path, long Length, long ModifiedTicks);

    private sealed class CacheLookupMemo
    {
        public CacheLookupProfile Profile { get; }
        private readonly ConcurrentDictionary<CacheFileIdentity, Lazy<LoadedParseCacheEntry?>> _entries = new();
        public ConcurrentDictionary<string, Lazy<bool>> CommonValidation { get; } = new(StringComparer.Ordinal);
        public ConcurrentDictionary<string, Lazy<bool>> ContextValidation { get; } = new(StringComparer.Ordinal);

        public CacheLookupMemo(CacheLookupProfile profile) => Profile = profile;

        public LoadedParseCacheEntry? Read(string path)
        {
            var file = new FileInfo(path);
            if (!file.Exists)
                return null;
            var canonical = OperatingSystem.IsWindows() ? file.FullName.ToUpperInvariant() : file.FullName;
            var identity = new CacheFileIdentity(canonical, file.Length, file.LastWriteTimeUtc.Ticks);
            var created = new Lazy<LoadedParseCacheEntry?>(() => Load(path));
            var stored = _entries.GetOrAdd(identity, created);
            if (!ReferenceEquals(created, stored))
                Profile.ReusedLoad();
            var result = stored.Value;
            if (result is null)
                ((ICollection<KeyValuePair<CacheFileIdentity, Lazy<LoadedParseCacheEntry?>>>)_entries)
                    .Remove(new(identity, stored));
            return result;
        }

        private LoadedParseCacheEntry? Load(string path)
        {
            try
            {
                string json;
                using (Profile.Measure(CacheOperation.Read))
                    json = File.ReadAllText(path);
                ParseCacheEnvelope? envelope;
                using (Profile.Measure(CacheOperation.Decode))
                    envelope = JsonSerializer.Deserialize<ParseCacheEnvelope>(json);
                if (envelope?.Payload is null)
                    return null;
                using (Profile.Measure(CacheOperation.Checksum))
                    if (Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(envelope.Payload))) != envelope.Checksum)
                        return null;
                ParseCacheEntry? entry;
                using (Profile.Measure(CacheOperation.Decode))
                    entry = JsonSerializer.Deserialize<ParseCacheEntry>(envelope.Payload);
                return entry is null ? null : new LoadedParseCacheEntry(envelope.Checksum, entry);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException)
            { return null; }
        }

        public void Invalidate(string path)
        {
            var canonical = Path.GetFullPath(path);
            if (OperatingSystem.IsWindows())
                canonical = canonical.ToUpperInvariant();
            foreach (var pair in _entries)
                if (pair.Key.Path == canonical)
                    _entries.TryRemove(pair.Key, out _);
        }
    }

    private sealed class SourceVisibilityAnalysis
    {
        public string Text { get; }
        private readonly Lazy<string[]> _lines;
        public string[] Lines => _lines.Value;
        public string[] ExportDefinitions { get; }
        public string[] ImportDefinitions { get; }
        public bool UnsupportedConstruction { get; }
        public ConcurrentDictionary<string, Lazy<bool>> MacroChecks { get; } = new(StringComparer.Ordinal);

        public SourceVisibilityAnalysis(string text)
        {
            Text = text;
            _lines = new(() => text.Split('\n'));
            ExportDefinitions = Definitions(text, "DLLEXPORT");
            ImportDefinitions = Definitions(text, "DLLIMPORT");
            UnsupportedConstruction = Regex.IsMatch(text,
                @"##[^\r\n]*EXPORT|EXPORT[^\r\n]*##|(?<!\w)_?EXPORT(?!\w)", RegexOptions.CultureInvariant);
        }

        private static string[] Definitions(string text, string alias)
            => Regex.Matches(text, $@"(?m)^\s*#\s*define\s+{alias}\b([^\r\n]*)", RegexOptions.CultureInvariant)
                .Cast<Match>().Select(match => match.Groups[1].Value).ToArray();
    }
}
