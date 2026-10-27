#include "MaterialShaderCompiler.h"
#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <set>
#include <cmath>
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Resources/ShaderReflectionMerge.h"
#include "Rendering/Resources/SpirvShaderReflection.h"
#include "Rendering/Resources/ParticleMaterialInterface.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"

namespace NLS::MaterialCompiler
{
namespace
{
std::optional<MaterialDependencySnapshot> FreezePassSources(const std::string& hlsl,
    const MaterialShaderCompileOptions& options,const Render::Assets::ShaderArtifact& contract,
    const Render::ShaderLab::ShaderLabPassDesc& pass,std::string& error)
{
    std::map<std::string,MaterialDependencySource> sources=options.dependencies ? options.dependencies->sources :
        std::map<std::string,MaterialDependencySource>{};
    auto normalize=[](const auto& path){return std::filesystem::absolute(path).lexically_normal();};
    const auto root=normalize(options.generatedPath).generic_string();
    sources[root]={root,normalize(options.generatedPath),hlsl,{},0,{}};
    std::vector<std::string> pending{root};
    std::set<std::string> diskInputs;
    const std::regex includePattern(R"nls(^\s*#\s*include\s*["<]([^">]+)[">])nls");
    const std::regex includeDirective(R"nls(^\s*#\s*include\b)nls");
    while (!pending.empty())
    {
        auto identity=std::move(pending.back()); pending.pop_back();
        auto& source=sources.at(identity);
        std::istringstream lines(source.content); std::string line;
        while (std::getline(lines,line))
        {
            std::smatch match;
            if (!std::regex_search(line,match,includePattern))
            {
                if (std::regex_search(line,includeDirective)) {error="Material includes must resolve to explicit files before snapshot capture";return std::nullopt;}
                continue;
            }
            const std::filesystem::path include=match[1].str();
            std::vector<std::filesystem::path> search{source.path.parent_path(),options.sourcePath.parent_path()};
            for (const auto& directory : options.includeDirectories) search.emplace_back(directory);
            std::filesystem::path resolved;
            std::optional<MaterialDependencySource> frozen;
            for (const auto& directory : search)
            {
                const auto candidate=normalize(include.is_absolute()?include:directory/include);
                const auto override=std::find_if(options.sourceOverrides.begin(),options.sourceOverrides.end(),[&](const auto& entry)
                    {return !entry.second.path.empty() && normalize(entry.second.path)==candidate;});
                if(override!=options.sourceOverrides.end()){resolved=candidate;frozen=override->second;break;}
                const auto supplied=std::find_if(sources.begin(),sources.end(),[&](const auto& entry)
                    {return !entry.second.path.empty() && normalize(entry.second.path)==candidate;});
                if (supplied!=sources.end()) {resolved=candidate;frozen=supplied->second;break;}
                if (std::filesystem::is_regular_file(candidate)) {resolved=candidate;break;}
            }
            if (resolved.empty()) {error="Missing material include: "+include.generic_string();return std::nullopt;}
            const auto key=resolved.generic_string(); source.dependencies.push_back(key);
            source.includes[include.generic_string()]=key;
            if (sources.contains(key)) continue;
            MaterialDependencySource captured;
            if (frozen) captured=*frozen;
            else
            {
                std::ifstream file(resolved,std::ios::binary);
                if (!file) {error="Cannot read material include: "+key;return std::nullopt;}
                captured.content.assign(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
                diskInputs.insert(key);
                std::ifstream metadata(Core::Assets::GetAssetMetaPath(resolved),std::ios::binary);
                if(metadata){const std::string bytes{std::istreambuf_iterator<char>(metadata),std::istreambuf_iterator<char>()};
                    captured.metadataHash=Core::Assets::ComputeNativeArtifactPayloadHash(reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size());}
            }
            captured.identity=key;captured.path=resolved;
            sources.emplace(key,std::move(captured)); pending.push_back(key);
            if (sources.size()>16384) {error="Material dependency count exceeds its limit";return std::nullopt;}
        }
    }
    std::string context=Render::ShaderCompiler::BuildShaderCompilerToolchainDependencyFingerprint();
    context+=Render::Resources::SerializeMaterialParameterSchema(contract.parameterSchema);
    const auto contractBytes=Render::Assets::SerializeShaderArtifact(contract);
    context.append(reinterpret_cast<const char*>(contractBytes.data()),contractBytes.size());
    context+=";vertex="+pass.vertexEntry+";fragment="+pass.fragmentEntry+";compute="+pass.computeEntry;
    context+=";glsl="+std::to_string(options.generateGlsl);
    for (const auto target : options.targets) context+=";target="+std::to_string(static_cast<int>(target));
    for (const auto& keywords : options.variants) context+=";keywords="+std::to_string(keywords.Hash());
    if (options.dependencies) context+=";base="+options.dependencies->Id();
    for (auto& [key,source] : sources) sources[root].dependencies.push_back(key==root?std::string{}:key);
    for (const auto& key : diskInputs)
    {
        std::ifstream currentFile(sources.at(key).path,std::ios::binary);
        const std::string current{std::istreambuf_iterator<char>(currentFile),std::istreambuf_iterator<char>()};
        if (!currentFile || current!=sources.at(key).content)
        {error="Material include changed while capturing dependencies: "+key;return std::nullopt;}
    }
    auto& dependencies=sources[root].dependencies;
    dependencies.erase(std::remove(dependencies.begin(),dependencies.end(),std::string{}),dependencies.end());
    const auto provider=[&](const std::string& key)->std::optional<MaterialDependencySource>
    {const auto it=sources.find(key);return it==sources.end()?std::nullopt:std::optional{it->second};};
    return MaterialDependencySnapshot::Capture({root},provider,std::move(context),error);
}
}
Render::Resources::MaterialParameterSchema BuildMaterialParameterSchema(
    const Render::ShaderLab::ShaderLabAssetDesc& shader, std::string_view identity)
{
    using namespace Render;
    Resources::MaterialParameterSchema schema;
    for (const auto& property : shader.properties)
    {
        Resources::MaterialParameterDesc desc;
        desc.id = Resources::MakeMaterialParameterId(identity,property.name);
        desc.name = property.name; desc.displayName = property.displayName;
        using Type = ShaderLab::ShaderLabPropertyType;
        using Uniform = Resources::UniformType;
        switch (property.type)
        {
        case Type::Int: desc.type = Uniform::UNIFORM_INT; break;
        case Type::Color: desc.color = true; [[fallthrough]];
        case Type::Vector: desc.type = Uniform::UNIFORM_FLOAT_VEC4; break;
        case Type::Texture2D: desc.type = Uniform::UNIFORM_SAMPLER_2D; break;
        case Type::TextureCube: desc.type = Uniform::UNIFORM_SAMPLER_CUBE; break;
        case Type::Range: desc.range = std::array{property.rangeMin,property.rangeMax}; [[fallthrough]];
        default: desc.type = Uniform::UNIFORM_FLOAT; break;
        }
        std::visit([&desc](const auto& value) { desc.defaultValue = value; },property.defaultValue);
        schema.parameters.push_back(std::move(desc));
    }
    for(const auto& sub:shader.subShaders)for(const auto& pass:sub.passes)
        for(const auto& feature:pass.shaderFeatures)for(const auto& keyword:feature.keywords)
        {
            if(keyword.empty() || keyword=="_" || schema.Find(keyword))continue;
            Resources::MaterialParameterDesc desc;
            desc.id=Resources::MakeMaterialParameterId(identity,"keyword:"+keyword);
            desc.name=keyword;desc.displayName=keyword;desc.group="Static switches";
            desc.type=Resources::UniformType::UNIFORM_BOOL;desc.defaultValue=false;desc.isStatic=true;
            schema.parameters.push_back(std::move(desc));
        }
    return schema;
}

Render::Assets::ShaderArtifact CompileMaterialPass(const Render::ShaderLab::ShaderLabPassDesc& pass,
    const MaterialShaderCompileOptions& options, const Render::Resources::MaterialParameterSchema& schema)
{
    using namespace Render;
    Assets::ShaderArtifact artifact;
    artifact.sourcePath = options.sourceIdentity.empty() ? options.sourcePath.generic_string() : options.sourceIdentity;
    artifact.subAssetKey = options.subAssetKey;
    artifact.parameterSchema = schema;
    artifact.shaderLabPassState = pass.state;
    if(auto it=pass.tags.values.find("MaterialDomain");it!=pass.tags.values.end())artifact.materialInterface.domain=it->second;
    if(auto it=pass.tags.values.find("MaterialRenderPath");it!=pass.tags.values.end())artifact.materialInterface.forwardOpaque=it->second=="ForwardOpaque";
    if(auto it=pass.tags.values.find("ShadingModel");it!=pass.tags.values.end())artifact.materialInterface.unlit=it->second=="Unlit";
    if(auto it=pass.tags.values.find("MaximumWorldDisplacement");it!=pass.tags.values.end())
    {try{artifact.materialInterface.maximumWorldDisplacement=std::stof(it->second);
        artifact.materialInterface.usesWorldPositionOffset=artifact.materialInterface.maximumWorldDisplacement>0;}catch(const std::exception&){artifact.materialInterface.maximumWorldDisplacement=-1;} }
    if(auto it=pass.tags.values.find("BlendMode");it!=pass.tags.values.end())
    {try{const auto value=std::stoi(it->second);artifact.materialInterface.blendMode=value>=0 && value<=4?static_cast<Resources::MaterialBlendMode>(value):static_cast<Resources::MaterialBlendMode>(255);}catch(const std::exception&){artifact.materialInterface.blendMode=static_cast<Resources::MaterialBlendMode>(255);} }
    if (const auto lightMode = pass.tags.values.find("LightMode"); lightMode != pass.tags.values.end())
        artifact.shaderLabLightMode = lightMode->second;
    auto fail = [&artifact](std::string error)
    {
        Assets::ShaderArtifactStage stage;
        stage.output.status = ShaderCompiler::ShaderCompilationStatus::Failed;
        stage.output.diagnostics = std::move(error); artifact.stages.push_back(std::move(stage));
    };
    if (artifact.shaderLabLightMode == "ParticleSprite" &&
        (!pass.vertexEntry.empty() || !pass.computeEntry.empty() || pass.fragmentEntry != "ParticleMaterialPS"))
    { fail("ParticleSprite requires only ParticleMaterialPS; the engine supplies the vertex shader"); return artifact; }
    auto variants = options.variants;
    if (variants.empty()) variants.emplace_back();
    if(pass.hlslSource.find("NLS_SKIN_ATTRIBUTES")!=std::string::npos)
    {const auto staticVariants=variants;for(auto variant:staticVariants){variant.Enable("NLS_SKINNED");variants.push_back(std::move(variant));}}
    std::sort(variants.begin(),variants.end(),[](const auto& a,const auto& b) {return a.ToVector()<b.ToVector();});
    variants.erase(std::unique(variants.begin(),variants.end(),[](const auto& a,const auto& b) {return a.ToVector()==b.ToVector();}),variants.end());
    if (variants.size()>options.variantBudget) { fail("Requested material variants exceed the configured budget"); return artifact; }
    if(!std::isfinite(artifact.materialInterface.maximumWorldDisplacement) || artifact.materialInterface.maximumWorldDisplacement<0 ||
        static_cast<int>(artifact.materialInterface.blendMode)>4)
    {fail("Invalid material rendering contract");return artifact;}
    for(const auto& variant:variants)for(const auto& feature:pass.shaderFeatures)
    {
        size_t enabled=0;
        for(const auto& keyword:feature.keywords)if(keyword!="_" && variant.Contains(keyword))++enabled;
        if(enabled>1){fail("A shader_feature group selected multiple mutually exclusive keywords");return artifact;}
    }
    std::string error;
    if (!schema.Validate(error)) { fail(error); return artifact; }
    const auto hlsl = ShaderLab::BuildShaderLabHlslForCompile(pass);
    auto frozen=FreezePassSources(hlsl,options,artifact,pass,error);
    if (!frozen) {fail(error);return artifact;}
    if (options.capturedDependencies) *options.capturedDependencies=std::make_shared<const MaterialDependencySnapshot>(*frozen);
    const auto generatedKey=std::filesystem::absolute(options.generatedPath).lexically_normal().generic_string();
    const auto codeProvider=[&](const std::string& key)->std::optional<MaterialDependencySource>
    {
        const auto found=frozen->sources.find(key);if(found==frozen->sources.end())return std::nullopt;
        auto source=found->second;source.metadataHash.clear();source.appliedGeneration=0;source.dependencies.clear();
        for(const auto& [name,path]:source.includes)source.dependencies.push_back(path);
        return source;
    };
    auto codeSnapshot=MaterialDependencySnapshot::Capture({generatedKey},codeProvider,"material-code-v1",error);
    if(!codeSnapshot){fail(error);return artifact;}
    const auto frozenRoot=options.generatedPath.parent_path()/"FrozenMaterialSources";
    if (!codeSnapshot->Stage(frozenRoot,error))
    { fail(error); return artifact; }
    artifact.dependencySnapshotId=frozen->Id();
    auto archiveId=artifact.dependencySnapshotId;archiveId=archiveId.substr(archiveId.find(':')+1);
    if(!frozen->Save(options.generatedPath.parent_path()/"MaterialSnapshots"/(archiveId+".json"),error))
    {fail(error);return artifact;}
    ShaderCompiler::ShaderCompiler compiler;
    compiler.SetCacheDatabasePath(options.cacheDatabasePath);
    const auto targets = options.targets.empty() ? std::vector{ShaderCompiler::ShaderTargetPlatform::DXIL,
        ShaderCompiler::ShaderTargetPlatform::SPIRV} : options.targets;
    for (const auto& variant : variants)
        for (const auto target : targets)
        {
            Resources::ShaderVariantReflection reflected{target,variant.Hash(),{}};
            const std::array entries = {std::pair{ShaderCompiler::ShaderStage::Vertex,pass.vertexEntry},
                std::pair{ShaderCompiler::ShaderStage::Pixel,pass.fragmentEntry},
                std::pair{ShaderCompiler::ShaderStage::Compute,pass.computeEntry}};
            for (const auto& [stage,entry] : entries)
            {
                if (entry.empty()) continue;
                ShaderCompiler::ShaderCompilationInput input;
                input.assetPath = codeSnapshot->StagePath(frozenRoot,options.generatedPath).string(); input.stage = stage;
                input.options.targetPlatform = target; input.options.entryPoint = entry;
                input.options.artifactDirectory=(options.generatedPath.parent_path()/"Bytecode").string();
                input.options.targetProfile = stage == ShaderCompiler::ShaderStage::Vertex ? "vs_6_0" :
                    stage == ShaderCompiler::ShaderStage::Pixel ? "ps_6_0" : "cs_6_0";
                input.options.includeDirectories={codeSnapshot->StagePath(frozenRoot,options.generatedPath).parent_path().string()};
                input.options.macros = ShaderLab::BuildShaderLabKeywordMacros(variant);
                auto output = compiler.Compile(input);
                for (auto& dependency : output.dependencyPaths)
                    for (const auto& [key,source] : codeSnapshot->sources)
                        if (!source.path.empty() && std::filesystem::path(dependency).lexically_normal()==
                            codeSnapshot->StagePath(frozenRoot,source.path).lexically_normal())
                        {dependency=std::filesystem::absolute(source.path).lexically_normal()==
                            std::filesystem::absolute(options.generatedPath).lexically_normal()
                            ? options.sourcePath.generic_string() : source.path.generic_string();break;}
                if (output.status != ShaderCompiler::ShaderCompilationStatus::Succeeded || output.bytecode.empty())
                { fail(output.diagnostics); return artifact; }
                Resources::ShaderReflection stageReflection;
                if (target == ShaderCompiler::ShaderTargetPlatform::SPIRV)
                {
                    if (!Resources::ReflectSpirvShader(output.bytecode,stage,stageReflection,error))
                    { fail(error); return artifact; }
                }
                else stageReflection = compiler.Reflect(input,output);
                if (!Resources::TryMergeShaderReflection(reflected.reflection,stageReflection,&error))
                { fail(error); return artifact; }
                artifact.stages.push_back({stage,target,entry,input.options.targetProfile,std::move(output),variant.Hash()});
            }
            if (artifact.shaderLabLightMode == "ParticleSprite" && !Resources::ValidateParticleSpriteReflection(reflected.reflection,error))
            { fail(error); return artifact; }
            // Engine Frame/Object sets are supplied by existing fixed providers.
            // Material-space registers may vary freely; switching the engine
            // object ABI between keyword variants would select the wrong provider.
            const auto indexed=[](const auto& reflection)
            {return std::any_of(reflection.properties.begin(),reflection.properties.end(),[](const auto& p)
                {return p.bindingSpace==3 && p.kind==Resources::ShaderResourceKind::StructuredBuffer && p.name=="ObjectData";});};
            const auto indirect=[](const auto& reflection)
            {return std::any_of(reflection.properties.begin(),reflection.properties.end(),[](const auto& p)
                {return p.bindingSpace==3 && p.kind==Resources::ShaderResourceKind::StructuredBuffer && p.name=="InstanceObjectIndices";});};
            for(const auto& previous:artifact.variantReflections)
                if(previous.targetPlatform==target && (indexed(previous.reflection)!=indexed(reflected.reflection) ||
                    indirect(previous.reflection)!=indirect(reflected.reflection)))
                {fail("Material variants must preserve the engine Object binding ABI");return artifact;}
            artifact.variantReflections.push_back(std::move(reflected));
        }
    if (!artifact.variantReflections.empty()) artifact.reflection = artifact.variantReflections.front().reflection;
    if (options.generateGlsl) Assets::AppendGlslShaderArtifactStages(artifact);
    return artifact;
}
}
