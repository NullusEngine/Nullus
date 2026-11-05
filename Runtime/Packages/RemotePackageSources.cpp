#include "RemotePackageSources.h"
#include "Plugins/RuntimeDeployment.h"
#include "Platform/Process/ProcessRunner.h"
#include "Core/Assets/PluginAssetPath.h"
#include "Guid.h"
#include <Json/json.hpp>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <array>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace NLS::Packages
{
namespace
{
using Platform::Process::ProcessRunOptions;
using Platform::Process::ProcessRunResult;
std::string Utf8(const std::filesystem::path& path)
{ const auto value = path.generic_u8string(); return {reinterpret_cast<const char*>(value.data()),value.size()}; }
std::string Lower(std::string value)
{ std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; }); return value; }
std::string Read(const std::filesystem::path& path, uint64_t limit)
{
    if (std::filesystem::file_size(path) > limit) throw std::runtime_error("remote package metadata size limit exceeded");
    std::ifstream input(path,std::ios::binary); if (!input) throw std::runtime_error("cannot read downloaded package metadata");
    return std::string((std::istreambuf_iterator<char>(input)),{});
}
bool Hash(const std::string& value)
{ return value.size() == 64 && std::all_of(value.begin(),value.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
struct Url { std::string origin, path; };
Url ParseUrl(const std::string& url)
{
    if (url.size() > 16384 || url.find_first_of("\r\n\t #\\") != std::string::npos || url.find('\0') != std::string::npos || Lower(url.substr(0,8)) != "https://")
        throw std::invalid_argument("package URL must be an HTTPS URL without credentials or fragments");
    const auto slash = url.find('/',8); const auto authority = url.substr(8,slash == std::string::npos ? slash : slash - 8);
    static const std::regex host(R"(^([A-Za-z0-9.-]+|\[[0-9A-Fa-f:]+\])(:[0-9]{1,5})?$)");
    if (!std::regex_match(authority,host)) throw std::invalid_argument("invalid HTTPS package URL authority");
    return {"https://" + Lower(authority),slash == std::string::npos ? "/" : url.substr(slash)};
}
std::string AbsoluteUrl(const std::string& base, const std::string& location)
{
    if (Lower(location.substr(0,8)) == "https://") { ParseUrl(location); return location; }
    const auto parsed = ParseUrl(base);
    if (location.empty() || location.rfind("//",0) == 0 || location.find(':') != std::string::npos) throw std::invalid_argument("invalid package redirect URL");
    return parsed.origin + (location.front() == '/' ? location : parsed.path.substr(0,parsed.path.find_last_of('/') + 1) + location);
}
bool Allowed(const std::string& initial, const std::string& target, const RemotePackageOptions& options)
{
    const auto origin = ParseUrl(target).origin;
    if (origin == ParseUrl(initial).origin) return true;
    for (const auto& allowed : options.allowedRedirectOrigins) if (origin == ParseUrl(allowed).origin) return true;
    return false;
}
std::map<std::string,std::string> Environment(const std::filesystem::path& work, const std::filesystem::path& tool)
{
    std::map<std::string,std::string> result;
#ifdef _WIN32
    wchar_t windows[32768]{}; if (!GetWindowsDirectoryW(windows,32768)) throw std::runtime_error("cannot resolve Windows directory");
    const auto root = std::filesystem::path(windows);
    result["SystemRoot"] = Utf8(root); result["PATH"] = Utf8(tool.parent_path()) + ";" + Utf8(root / "System32");
    result["TEMP"] = Utf8(work); result["TMP"] = Utf8(work);
#else
    result["PATH"] = Utf8(tool.parent_path()) + ":/usr/bin:/bin"; result["TMPDIR"] = Utf8(work);
    result["LANG"] = "C.UTF-8";
#endif
    return result;
}
void Check(const ProcessRunResult& result, const char* action)
{
    if (result.Succeeded()) return;
    if (result.cancelled) throw std::runtime_error("package operation cancelled");
    if (result.outputLimitExceeded) throw std::runtime_error(std::string(action) + ": package output size limit exceeded");
    if (result.timedOut) throw std::runtime_error(std::string(action) + ": timeout");
    throw std::runtime_error(std::string(action) + ": " + result.error);
}
void Download(const RemotePackageOptions& options, const std::string& initial, const std::string& requested,
    const std::filesystem::path& file, uint64_t maximum, const std::atomic_bool* cancel)
{
    if (options.curlExecutable.empty()) throw std::runtime_error("curl executable is not configured");
    auto current = requested;
    for (unsigned redirects = 0; redirects <= 3; ++redirects)
    {
        if (!Allowed(initial,current,options)) throw std::runtime_error("package redirect/archive origin is not allowed");
        const auto headers = file.parent_path() / (file.filename().string() + ".headers");
        ProcessRunOptions run; run.executable = options.curlExecutable; run.workingDirectory = file.parent_path();
        run.environment = Environment(file.parent_path(),run.executable); run.stdoutFile = file; run.maxOutputBytes = maximum; run.cancel = cancel;
        run.arguments = {"--disable","--silent","--show-error","--globoff","--proto","=https","--proto-redir","=https",
            "--max-redirs","0","--connect-timeout","30","--max-time","600","--max-filesize",std::to_string(maximum),
            "--noproxy","*","--dump-header",Utf8(headers),"--output","-"};
        if (!options.caCertificate.empty()) { run.arguments.push_back("--cacert"); run.arguments.push_back(Utf8(options.caCertificate)); }
        run.arguments.push_back("--url"); run.arguments.push_back(current);
        Check(Platform::Process::Run(run),"HTTPS package download");
        std::istringstream input(Read(headers,1024*1024)); std::string line, location; int status = 0;
        while (std::getline(input,line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("HTTP/",0) == 0) { const auto space = line.find(' '); status = space == std::string::npos ? 0 : std::stoi(line.substr(space+1,3)); location.clear(); }
            else if (Lower(line.substr(0,9)) == "location:") { location = line.substr(9); while (!location.empty() && location.front() == ' ') location.erase(location.begin()); }
        }
        if (status == 200) return;
        if (status >= 300 && status < 400 && !location.empty() && redirects < 3) { current = AbsoluteUrl(current,location); continue; }
        throw std::runtime_error("HTTPS package server returned status " + std::to_string(status));
    }
    throw std::runtime_error("package redirect limit exceeded");
}
}
GitPackageSource::GitPackageSource(RemotePackageOptions options) : m_options(std::move(options)) {}
void GitPackageSource::Fetch(const PackageRequest& request, const std::filesystem::path& destination, const PackageLimits& limits)
{
    ParseUrl(request.location);
    if ((request.revision.size() != 40 && request.revision.size() != 64) || !std::all_of(request.revision.begin(),request.revision.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::invalid_argument("Git packages require a full immutable commit hash");
    if (m_options.gitExecutable.empty()) throw std::runtime_error("Git executable is not configured");
    const auto work = destination.parent_path() / ".git-source"; const auto repository = work / "repository";
    std::filesystem::create_directories(work / "empty-template"); std::filesystem::create_directories(work / "empty-hooks");
    std::ofstream(work / "empty-config",std::ios::binary).close();
    auto environment = Environment(work,m_options.gitExecutable);
    environment["GIT_CONFIG_NOSYSTEM"] = "1"; environment["GIT_CONFIG_SYSTEM"] = Utf8(work / "empty-config");
    environment["GIT_CONFIG_GLOBAL"] = Utf8(work / "empty-config"); environment["GIT_TERMINAL_PROMPT"] = "0";
    environment["GIT_ALLOW_PROTOCOL"] = "https"; environment["GIT_LFS_SKIP_SMUDGE"] = "1"; environment["GCM_INTERACTIVE"] = "Never";
    std::vector<std::string> prefix{"-c","core.hooksPath=" + Utf8(work / "empty-hooks"),"-c","core.fsmonitor=false",
        "-c","credential.helper=","-c","credential.interactive=false","-c","core.askPass=","-c","protocol.allow=never",
        "-c","protocol.https.allow=always","-c","fetch.recurseSubmodules=false","-c","submodule.recurse=false",
        "-c","gc.auto=0","-c","maintenance.auto=false","-c","http.followRedirects=false","-c","http.sslVerify=true",
        "-c","transfer.fsckObjects=true","-c","fetch.fsckObjects=true"};
    if (!m_options.caCertificate.empty()) { prefix.push_back("-c"); prefix.push_back("http.sslCAInfo=" + Utf8(m_options.caCertificate)); }
    const auto execute = [&](std::vector<std::string> arguments, const std::filesystem::path& output = std::filesystem::path{}) {
        ProcessRunOptions run; run.executable = m_options.gitExecutable; run.environment = environment; run.workingDirectory = work; run.cancel = request.cancel;
        run.arguments = prefix; run.arguments.insert(run.arguments.end(),arguments.begin(),arguments.end());
        run.stdoutFile = output; run.maxOutputBytes = output.empty() ? 8*1024*1024 : limits.maxDownloadBytes;
        run.monitoredDirectory = repository; run.maxDirectoryBytes = limits.maxDownloadBytes;
        auto result = Platform::Process::Run(run); Check(result,"Git package operation"); return result.output;
    };
    auto init = std::vector<std::string>{"init","--bare","--template=" + Utf8(work / "empty-template")};
    if (request.revision.size() == 64) init.push_back("--object-format=sha256");
    init.push_back(Utf8(repository)); execute(std::move(init));
    execute({"-C",Utf8(repository),"fetch","--no-tags","--no-recurse-submodules","--depth=1","--no-write-fetch-head",request.location,request.revision});
    const auto type = execute({"-C",Utf8(repository),"cat-file","-t",request.revision});
    if (type != "commit\n" && type != "commit\r\n") throw std::runtime_error("Git package revision must identify a commit object");
    uint64_t received = 0;
    for (const auto& file : std::filesystem::recursive_directory_iterator(repository)) if (file.is_regular_file())
    { const auto size = file.file_size(); if (size > limits.maxDownloadBytes - received) throw std::runtime_error("Git package repository download limit exceeded"); received += size; }
    const auto tree = execute({"-C",Utf8(repository),"ls-tree","-rz","--full-tree",request.revision});
    size_t cursor = 0; uint32_t files = 0;
    while (cursor < tree.size())
    {
        const auto end = tree.find('\0',cursor); if (end == std::string::npos || ++files > limits.maxFiles) throw std::runtime_error("Git package tree limit exceeded");
        if (tree.compare(cursor,6,"160000") == 0) throw std::runtime_error("Git submodules are not supported by package installation");
        cursor = end + 1;
    }
    const auto archive = work / "package.zip";
    execute({"-C",Utf8(repository),"archive","--format=zip",request.revision},archive);
    PackageRequest zip{PackageSourceKind::Zip,Utf8(archive)}; zip.cancel = request.cancel;
    ZipPackageSource{}.Fetch(zip,destination,limits);
    for (const auto& file : std::filesystem::recursive_directory_iterator(destination)) if (file.is_regular_file())
    {
        std::ifstream input(file.path(),std::ios::binary); std::array<char,64> first{}; input.read(first.data(),first.size());
        constexpr std::string_view pointer="version https://git-lfs.github.com/spec/v1";
        if (input.gcount()>=static_cast<std::streamsize>(pointer.size()) && std::string_view(first.data(),pointer.size())==pointer)
            throw std::runtime_error("Git LFS packages are not supported; publish a complete ZIP instead");
    }
    std::filesystem::remove_all(work);
}
struct RegistryPackageSource::Impl
{
    explicit Impl(RemotePackageOptions value) : options(std::move(value)) {}
    RemotePackageOptions options;
    std::mutex mutex;
    std::map<std::string,Plugins::PluginManifest> catalog;
};
RegistryPackageSource::RegistryPackageSource(RemotePackageOptions options) : m_impl(std::make_unique<Impl>(std::move(options))) {}
RegistryPackageSource::~RegistryPackageSource() = default;
std::vector<Plugins::PluginManifest> RegistryPackageSource::Candidates(const PackageRequest& request)
{
    ParseUrl(request.location);
    const auto work = std::filesystem::temp_directory_path() / ("NullusRegistry-" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(work);
    struct Guard { std::filesystem::path path; ~Guard() { std::error_code ignored; std::filesystem::remove_all(path,ignored); } } guard{work};
    const auto index = request.location.ends_with(".json") ? request.location : request.location + (request.location.ends_with('/') ? "index.json" : "/index.json");
    Download(m_impl->options,request.location,index,work / "index.json",8*1024*1024,request.cancel);
    const auto json = nlohmann::json::parse(Read(work / "index.json",8*1024*1024));
    if (json.at("schemaVersion").get<uint32_t>() != 1) throw std::runtime_error("unsupported Nullus Registry schema");
    std::vector<Plugins::PluginManifest> result;
    for (const auto& entry : json.at("packages")) for (const auto& version : entry.at("versions"))
    {
        auto manifest = Plugins::PluginManifest::Parse(version.at("manifest").dump(),work / "metadata");
        if (manifest.id != entry.at("id").get<std::string>() || manifest.version.String() != version.at("version").get<std::string>()) throw std::runtime_error("registry metadata identity mismatch");
        manifest.source = request.location; manifest.distributionHash = version.at("archive").at("sha256").get<std::string>();
        if (!Hash(manifest.distributionHash)) throw std::runtime_error("registry archive SHA-256 is invalid");
        manifest.distributionUrl = AbsoluteUrl(index,version.at("archive").at("url").get<std::string>());
        if (!Allowed(request.location,manifest.distributionUrl,m_impl->options)) throw std::runtime_error("registry archive origin is not allowed");
        result.push_back(std::move(manifest));
    }
    { std::lock_guard lock(m_impl->mutex); for (const auto& manifest : result) m_impl->catalog[request.location + "|" + manifest.id + "@" + manifest.version.String()] = manifest; }
    return result;
}
void RegistryPackageSource::Fetch(const PackageRequest& request, const std::filesystem::path& destination, const PackageLimits& limits)
{
    if (!Plugins::ValidPluginId(request.id)) throw std::invalid_argument("Registry packages require a plugin ID");
    auto candidates = Candidates(request);
    auto resolved = Plugins::ResolvePlugins({{request.id,request.version,request.location}},candidates,Plugins::HostPlatform());
    if (!resolved.succeeded) throw std::runtime_error(resolved.error);
    const auto selected = std::find_if(resolved.ordered.begin(),resolved.ordered.end(),[&](const auto& p) { return p.id == request.id; });
    if (selected == resolved.ordered.end()) throw std::runtime_error("registry package version was not found");
    if (!request.expectedArchiveHash.empty() && selected->distributionHash != request.expectedArchiveHash) throw std::runtime_error("registry metadata changed during package resolution");
    const auto archive = destination.parent_path() / "download.zip";
    Download(m_impl->options,request.location,selected->distributionUrl,archive,limits.maxDownloadBytes,request.cancel);
    if (Plugins::HashPluginFile(archive) != selected->distributionHash) throw std::runtime_error("registry archive integrity mismatch (SHA-256)");
    PackageRequest zip{PackageSourceKind::Zip,Utf8(archive)}; zip.cancel = request.cancel;
    ZipPackageSource{}.Fetch(zip,destination,limits);
    const auto actual = Plugins::PluginManifest::Read(destination / "plugin.json");
    if (actual.id != selected->id || actual.version != selected->version ||
        nlohmann::json::parse(actual.raw) != nlohmann::json::parse(selected->raw))
        throw std::runtime_error("registry archive manifest does not match resolved metadata");
    std::filesystem::remove(archive);
    std::error_code ignored; std::filesystem::remove(archive.parent_path() / "download.zip.headers",ignored);
}
RemotePackageOptions DefaultRemotePackageOptions()
{
    RemotePackageOptions options;
#ifdef NLS_PACKAGE_GIT
    options.gitExecutable = std::filesystem::u8path(NLS_PACKAGE_GIT);
#endif
#ifdef NLS_PACKAGE_CURL
    options.curlExecutable = std::filesystem::u8path(NLS_PACKAGE_CURL);
#endif
    return options;
}
void ConfigureRemotePackageSources(PackageManager& manager, RemotePackageOptions options)
{
    manager.SetProvider(PackageSourceKind::Git,std::make_shared<GitPackageSource>(options));
    manager.SetProvider(PackageSourceKind::Registry,std::make_shared<RegistryPackageSource>(std::move(options)));
}
}
