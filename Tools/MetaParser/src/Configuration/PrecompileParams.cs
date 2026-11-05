using System.Text.Json;

internal sealed class PrecompileParams
{
    public string RootDir { get; set; } = string.Empty;
    public string SourceDir { get; set; } = string.Empty;
    public string TargetName { get; set; } = string.Empty;
    public string ModuleName { get; set; } = string.Empty;
    public string OutputDir { get; set; } = string.Empty;
    public string CompilerPath { get; set; } = string.Empty;
    public string CompilerId { get; set; } = string.Empty;
    public string CompilerTarget { get; set; } = string.Empty;
    public string ResourceDir { get; set; } = string.Empty;
    public string Sysroot { get; set; } = string.Empty;
    public List<string> Headers { get; set; } = [];
    public List<string> IncludeDirs { get; set; } = [];
    public List<string> SystemIncludeDirs { get; set; } = [];
    public List<string> Defines { get; set; } = [];
    public List<string> CompilerOptions { get; set; } = [];
    public string ParseMode { get; set; } = "threads";
    public int ParseJobs { get; set; }
    public bool UseCache { get; set; } = true;
    public string CacheDirectory { get; set; } = string.Empty;
    public List<string> VisibilityDefines { get; set; } = [];
    public int TotalParseJobs { get; set; }

    public static PrecompileParams? Load(string path)
    {
        return JsonSerializer.Deserialize<PrecompileParams>(File.ReadAllText(path), new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true
        });
    }

    public bool IsValid()
    {
        return !string.IsNullOrWhiteSpace(RootDir)
               && !string.IsNullOrWhiteSpace(SourceDir)
               && !string.IsNullOrWhiteSpace(OutputDir);
    }
}
