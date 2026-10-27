#include "ShaderCooker.h"

#include "MaterialCompiler/MaterialShaderCompiler.h"

#include "Assets/ArtifactManifest.h"
#include "Assets/AssetMeta.h"
#include "Platform/Process/Process.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"
#include "Rendering/ShaderLab/ShaderLabVariant.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace NLS::Cook::Detail
{
namespace
{
std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto size = input.tellg();
    if (size <= 0)
        return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return input ? bytes : std::vector<uint8_t> {};
}

std::string HashFile(const std::filesystem::path& path)
{
    const auto bytes = ReadAllBytes(path);
    return bytes.empty()
        ? std::string()
        : NLS::Core::Assets::ToHexString(
            NLS::Core::Assets::ComputeSha256(bytes.data(), bytes.size()));
}

std::string NormalizeRelative(
    const std::filesystem::path& root,
    const std::filesystem::path& path)
{
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    return error ? path.generic_string() : relative.generic_string();
}

std::string SanitizePassKey(std::string value)
{
    if (value.empty())
        value = "Pass";
    for (auto& character : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(character)) &&
            character != '_' && character != '-')
        {
            character = '_';
        }
    }
    return value;
}

void AddKeywordOption(
    std::vector<NLS::Render::ShaderLab::ShaderLabKeywordSet>& variants,
    const NLS::Render::ShaderLab::ShaderLabKeywordPragma& pragma,
    const bool includeOffOption)
{
    using NLS::Render::ShaderLab::ShaderLabKeywordSet;

    std::vector<std::string> options;
    if (includeOffOption)
        options.emplace_back();
    for (const auto& keyword : pragma.keywords)
    {
        if (keyword == "_")
        {
            if (std::find(options.begin(), options.end(), std::string {}) == options.end())
                options.emplace_back();
            continue;
        }
        if (!keyword.empty() &&
            std::find(options.begin(), options.end(), keyword) == options.end())
        {
            options.push_back(keyword);
        }
    }
    if (options.empty())
        return;
    if (variants.size() > 4096u / options.size())
        throw std::runtime_error("ShaderLab variants exceed the hard allocation limit");

    std::vector<ShaderLabKeywordSet> expanded;
    expanded.reserve(variants.size() * options.size());
    for (const auto& base : variants)
    {
        for (const auto& keyword : options)
        {
            auto candidate = base;
            if (!keyword.empty())
                candidate.Enable(keyword);
            expanded.push_back(std::move(candidate));
        }
    }
    variants = std::move(expanded);
}

std::vector<NLS::Render::ShaderLab::ShaderLabKeywordSet> BuildKeywordVariants(
    const NLS::Render::ShaderLab::ShaderLabPassDesc& pass)
{
    using NLS::Render::ShaderLab::ShaderLabKeywordSet;

    std::vector<ShaderLabKeywordSet> variants;
    variants.emplace_back();
    for (const auto& pragma : pass.multiCompiles)
        AddKeywordOption(variants, pragma, false);
    for (const auto& pragma : pass.shaderFeatures)
        AddKeywordOption(variants, pragma, true);

    std::sort(
        variants.begin(),
        variants.end(),
        [](const ShaderLabKeywordSet& lhs, const ShaderLabKeywordSet& rhs)
        {
            return lhs.ToVector() < rhs.ToVector();
        });
    variants.erase(
        std::unique(
            variants.begin(),
            variants.end(),
            [](const ShaderLabKeywordSet& lhs, const ShaderLabKeywordSet& rhs)
            {
                return lhs.ToVector() == rhs.ToVector();
            }),
        variants.end());
    return variants;
}

bool HasFailedStage(const NLS::Render::Assets::ShaderArtifact& artifact)
{
    return std::any_of(
        artifact.stages.begin(),
        artifact.stages.end(),
        [](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.output.status !=
                    NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded ||
                stage.output.bytecode.empty();
        });
}

std::string CollectStageDiagnostics(const NLS::Render::Assets::ShaderArtifact& artifact)
{
    std::ostringstream stream;
    for (const auto& stage : artifact.stages)
    {
        if (!stage.output.diagnostics.empty())
            stream << stage.output.diagnostics << "\n";
    }
    return stream.str();
}
} // namespace

bool CookShaderAsset(
    const CookOptions& options,
    const std::filesystem::path& source,
    const NLS::Core::Assets::AssetMeta& meta,
    ShaderCookOutput& output,
    std::string& error)
{
    using namespace NLS::Render;
    namespace Assets = NLS::Render::Assets;

    const auto sourceBytes = ReadAllBytes(source);
    if (sourceBytes.empty())
    {
        error = "shader source is empty or unreadable";
        return false;
    }
    const std::string sourceText(
        reinterpret_cast<const char*>(sourceBytes.data()),
        sourceBytes.size());
    const auto parsed = ShaderLab::ParseShaderLabSource(
        sourceText,
        source.generic_string());
    if (!parsed.Succeeded())
    {
        error = parsed.DiagnosticsToString();
        return false;
    }

    std::vector<const ShaderLab::ShaderLabPassDesc*> passes;
    const ShaderLab::ShaderLabPassDesc* primaryPass = nullptr;
    for (const auto& subShader : parsed.asset.subShaders)
    {
        for (const auto& pass : subShader.passes)
        {
            const auto lightMode = pass.tags.values.find("LightMode");
            if (lightMode == pass.tags.values.end() || lightMode->second.empty())
                continue;
            passes.push_back(&pass);
            if (primaryPass == nullptr ||
                (lightMode->second == "Forward" &&
                    primaryPass->tags.values.find("LightMode")->second != "Forward"))
            {
                primaryPass = &pass;
            }
        }
    }
    if (primaryPass == nullptr)
    {
        error = "ShaderLab asset has no LightMode pass";
        return false;
    }

    const auto sourceRelative = NormalizeRelative(options.projectRoot, source);
    const auto stem = source.stem().generic_string();
    const auto baseSubAssetKey = "shader:" + stem;
    const auto parameterSchema = MaterialCompiler::BuildMaterialParameterSchema(
        parsed.asset,
        meta.id.ToString());

    const auto cacheRoot = options.projectRoot / "Library" / "ShaderCache" / "Cook";
    std::vector<std::string> includeDirectories;
    const auto appendIncludeDirectory = [&includeDirectories](const std::filesystem::path& directory)
    {
        if (directory.empty())
            return;
        const auto normalized = directory.lexically_normal().string();
        if (std::find(includeDirectories.begin(), includeDirectories.end(), normalized) ==
            includeDirectories.end())
        {
            includeDirectories.push_back(normalized);
        }
    };
    appendIncludeDirectory(source.parent_path());
    appendIncludeDirectory(source.parent_path().parent_path());
    appendIncludeDirectory(options.assetsRoot / "Engine" / "Shaders");
#if defined(NLS_ROOT_DIR)
    appendIncludeDirectory(std::filesystem::path(NLS_ROOT_DIR) / "App" / "Assets" / "Engine" / "Shaders");
#endif
    const auto resourceRoots = NLS::Platform::Process::ResolveInstallResourceRoots();
    appendIncludeDirectory(resourceRoots.engineAssetsRoot / "Shaders");

    ShaderCompiler::ShaderTargetPlatform targetPlatform =
        ShaderCompiler::ShaderTargetPlatform::DXIL;
    if (options.target.shaderTarget == "spirv")
        targetPlatform = ShaderCompiler::ShaderTargetPlatform::SPIRV;
    else if (options.target.shaderTarget == "glsl")
        targetPlatform = ShaderCompiler::ShaderTargetPlatform::GLSL;

    size_t passOrdinal = 0u;
    for (const auto* pass : passes)
    {
        const auto lightMode = pass->tags.values.find("LightMode");
        const auto displayName = lightMode != pass->tags.values.end() && !lightMode->second.empty()
            ? lightMode->second
            : (!pass->name.empty() ? pass->name : "Pass");
        const auto subAssetKey = pass == primaryPass
            ? baseSubAssetKey
            : baseSubAssetKey + "/" + SanitizePassKey(
                pass->name.empty() ? displayName : pass->name) +
                "#" + std::to_string(passOrdinal);

        const auto passKey = SanitizePassKey(
            pass->name.empty() ? displayName : pass->name) +
            "_" + std::to_string(passOrdinal);
        const auto generatedPath = cacheRoot / (stem + "_" + passKey) / (passKey + ".hlsl");

        MaterialCompiler::MaterialShaderCompileOptions compileOptions;
        compileOptions.sourcePath = source;
        compileOptions.generatedPath = generatedPath;
        compileOptions.sourceIdentity = sourceRelative;
        compileOptions.subAssetKey = subAssetKey;
        compileOptions.cacheDatabasePath =
            (options.projectRoot / "Library" / "ShaderCache" / "ShaderCache.tsv").string();
        compileOptions.includeDirectories = includeDirectories;
        compileOptions.targets = {targetPlatform};
        compileOptions.variants = BuildKeywordVariants(*pass);
        compileOptions.variantBudget = 512u;
        compileOptions.generateGlsl = targetPlatform == ShaderCompiler::ShaderTargetPlatform::GLSL;

        std::shared_ptr<const MaterialCompiler::MaterialDependencySnapshot> dependencies;
        compileOptions.capturedDependencies = &dependencies;
        auto artifact = MaterialCompiler::CompileMaterialPass(
            *pass,
            compileOptions,
            parameterSchema);
        artifact.sourcePath = sourceRelative;
        artifact.subAssetKey = subAssetKey;
        artifact.targetPlatform = options.target.artifactPlatform;
        artifact.parameterSchema = parameterSchema;

        if (!Assets::HasUsableShaderArtifactStage(artifact) || HasFailedStage(artifact))
        {
            error = sourceRelative + ": ShaderLab pass \"" + displayName +
                "\" produced no usable " + options.target.shaderTarget + " stage";
            const auto stageDiagnostics = CollectStageDiagnostics(artifact);
            if (!stageDiagnostics.empty())
                error += "\n" + stageDiagnostics;
            return false;
        }

        if (dependencies)
        {
            for (const auto& [key, dependency] : dependencies->sources)
            {
                (void)key;
                if (dependency.path.empty())
                    continue;
                const auto relative = NormalizeRelative(options.projectRoot, dependency.path);
                if (relative.empty() || relative.rfind("..", 0u) == 0u)
                    continue;
                const auto hash = HashFile(dependency.path);
                if (!hash.empty())
                {
                    output.dependencies.push_back({
                        NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
                        relative,
                        hash});
                }
            }
        }

        auto payload = Assets::SerializeShaderArtifact(artifact);
        if (payload.empty())
        {
            error = sourceRelative + ": shader artifact serialization failed";
            return false;
        }

        NLS::Core::Assets::ArtifactPayload artifactPayload;
        artifactPayload.subAssetKey = subAssetKey;
        artifactPayload.artifactType = NLS::Core::Assets::ArtifactType::Shader;
        artifactPayload.loaderId = "shader";
        artifactPayload.displayName = stem + " " + displayName;
        artifactPayload.relativePath =
            std::filesystem::path("shaders") / (stem + "_" + passKey + ".shader");
        artifactPayload.payload = std::move(payload);
        artifactPayload.dependencies = output.dependencies;
        output.payloads.push_back(std::move(artifactPayload));

        if (pass == primaryPass)
            output.primarySubAssetKey = subAssetKey;
        ++passOrdinal;
    }

    if (output.payloads.empty())
    {
        error = "shader asset produced no artifacts";
        return false;
    }
    return true;
}
} // namespace NLS::Cook::Detail
