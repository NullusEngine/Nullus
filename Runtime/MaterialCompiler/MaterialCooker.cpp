#include "MaterialCooker.h"
#include <algorithm>
#include <fstream>
#include <set>
#include "Assets/ArtifactDatabase.h"
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Assets/TextureArtifact.h"

namespace NLS::MaterialCompiler
{
MaterialCookResult CookMaterialFamily(const Render::ShaderLab::ShaderLabAssetDesc& shader,
    const Render::Resources::MaterialParameterSchema& schema,const std::vector<MaterialCookEntry>& materials,
    const MaterialCookOptions& options)
{
    namespace A=Core::Assets;
    MaterialCookResult result;
    try
    {
        if(materials.empty() || !options.shaderId.IsValid() || options.outputRoot.empty())
        {result.error="Material publication needs identified materials, a shader and an output folder";return result;}
        const auto databasePath=options.outputRoot/"Library"/"ArtifactDB";
        if(std::filesystem::exists(databasePath)){result.error="Material release already exists; choose a new release folder";return result;}
        auto compile=options.compiler;
        compile.variants=options.variantCollection;
        std::vector<Render::Assets::ResolvedMaterialAsset> resolved;
        std::set<A::AssetId> materialIds;
        for(const auto& material:materials)
        {
            if(!material.id.IsValid() || !materialIds.insert(material.id).second)
            {result.error="Material publication has an empty or duplicate material identity";return result;}
            auto asset=material.resolved;
            for(const auto& parameter:schema.parameters)
            {
                if(parameter.isStatic && std::holds_alternative<bool>(parameter.defaultValue))
                    asset.staticParameters.try_emplace(parameter.name,std::get<bool>(parameter.defaultValue));
                if(!parameter.isStatic && !std::holds_alternative<std::monostate>(parameter.defaultValue))
                    asset.parameters.try_emplace(parameter.name,Render::Assets::MaterialParameterOverride{parameter.id,parameter.name,parameter.type,parameter.defaultValue});
            }
            Render::ShaderLab::ShaderLabKeywordSet keywords;
            for(const auto& [name,value]:asset.staticParameters)if(value)keywords.Enable(name);
            compile.variants.push_back(std::move(keywords));resolved.push_back(std::move(asset));
        }
        std::sort(compile.variants.begin(),compile.variants.end(),[](const auto& a,const auto& b){return a.ToVector()<b.ToVector();});
        compile.variants.erase(std::unique(compile.variants.begin(),compile.variants.end(),[](const auto& a,const auto& b){return a.ToVector()==b.ToVector();}),compile.variants.end());
        if(compile.variants.size()>compile.variantBudget){result.error="Used material variants exceed the configured budget";return result;}
        struct Pending{A::ArtifactWriteRequest request;std::string source;};
        std::vector<Pending> pending;
        Pending program;program.source=compile.sourceIdentity;program.request.sourceAssetId=options.shaderId;
        program.request.importerId="material-cooker";program.request.importerVersion=1;
        program.request.targetPlatform=options.targetPlatform;
        MaterialDependencySnapshot closure;if(compile.dependencies)closure=*compile.dependencies;
        for(const auto& sub:shader.subShaders)for(const auto& pass:sub.passes)
        {
            if(!pass.tags.values.contains("LightMode"))continue;
            auto passOptions=compile;
            passOptions.subAssetKey="shader:"+pass.name;
            passOptions.generatedPath=compile.generatedPath.parent_path()/(std::to_string(Render::Resources::MakeMaterialParameterId(compile.sourceIdentity,pass.name))+".hlsl");
            std::shared_ptr<const MaterialDependencySnapshot> dependencies;
            passOptions.capturedDependencies=&dependencies;
            auto artifact=CompileMaterialPass(pass,passOptions,schema);
            artifact.targetPlatform=options.targetPlatform;
            if(artifact.stages.empty()){result.error="Material pass produced no executable stages";return result;}
            for(const auto& stage:artifact.stages)if(stage.output.status!=Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
            {result.error=pass.name+": "+stage.output.diagnostics;return result;}
            if(dependencies)for(const auto& [key,source]:dependencies->sources)
            {
                if(const auto old=closure.sources.find(key);old!=closure.sources.end() && old->second.content!=source.content)
                {result.error="Material dependencies changed between cooked passes";return result;}
                closure.sources[key]=source;
            }
            closure.context+=';'+artifact.dependencySnapshotId;
            if(program.request.primarySubAssetKey.empty() || artifact.shaderLabLightMode=="Forward" || artifact.shaderLabLightMode=="ParticleSprite")
                program.request.primarySubAssetKey=passOptions.subAssetKey;
            program.request.artifacts.emplace_back(passOptions.subAssetKey,A::ArtifactType::Shader,"shader",pass.name,"shader",Render::Assets::SerializeShaderArtifact(artifact));
        }
        if(program.request.artifacts.empty()){result.error="Material shader has no runtime passes";return result;}
        pending.push_back(std::move(program));
        A::ArtifactWriter writer(options.outputRoot/"Library"/"Staging",options.outputRoot/"Library"/"Artifacts");
        A::ArtifactDatabase database;
        // Textures are already imported native artifacts. Re-publish them by
        // content, rewrite all material references and retain their typed GUIDs.
        std::map<std::string,std::pair<A::AssetId,std::string>> textureRefs;
        for(auto& asset:resolved)for(auto& [name,parameter]:asset.parameters)
        {
            using U=Render::Resources::UniformType;
            if(parameter.type!=U::UNIFORM_SAMPLER_2D && parameter.type!=U::UNIFORM_SAMPLER_CUBE)continue;
            auto* path=std::get_if<std::string>(&parameter.value);if(!path || path->empty() || *path=="white" || *path=="black" || *path=="bump")continue;
            if(!textureRefs.contains(*path))
            {
                auto texture=options.frozenTextures.contains(*path)?std::optional{options.frozenTextures.at(*path)}:
                    options.textureProvider?options.textureProvider(*path):std::nullopt;
                if(!texture || !texture->id.IsValid() || !Render::Assets::DeserializeTextureArtifact(texture->nativeArtifact))
                {result.error="Publication cannot resolve native texture: "+*path;return result;}
                const auto key="texture:"+*path;
                closure.sources[key]={key,{},std::string(texture->nativeArtifact.begin(),texture->nativeArtifact.end()),{},0,{}};
                A::ArtifactWriteRequest request;request.sourceAssetId=texture->id;request.importerId="material-texture-copy";
                request.targetPlatform=options.targetPlatform;request.primarySubAssetKey=texture->subAssetKey;
                request.artifacts.emplace_back(texture->subAssetKey,A::ArtifactType::Texture,"texture","texture",texture->nativeArtifact);
                const auto committed=writer.WriteAndCommit(request,nullptr);
                if(!committed.committed){result.error="Cannot publish material texture";return result;}
                const auto* entry=committed.manifest.FindPrimaryArtifact();if(!entry){result.error="Texture publication has no primary artifact";return result;}
                textureRefs[*path]={texture->id,entry->artifactPath};
                auto manifest=committed.manifest;
                textureRefs[*path].second=manifest.FindPrimaryArtifact()->artifactPath;
                result.manifests.push_back(manifest);database.UpsertManifest(manifest,*path,A::ArtifactRecordStatus::UpToDate);
            }
            *path=textureRefs.at(*path).second;
        }
        for(size_t i=0;i<materials.size();++i)
        {
            auto& asset=resolved[i];asset.shader.path=compile.sourceIdentity;
            // Cook only recognized current bindings; obsolete authoring values
            // remain in the source .mat and are absent from runtime payloads.
            std::map<std::string,Render::Assets::MaterialParameterOverride> values;
            for(const auto& [name,value]:asset.parameters)
            {const auto* definition=value.id?schema.Find(value.id):schema.Find(name);
                if(definition && definition->type==value.type){auto mapped=value;mapped.name=definition->name;values[mapped.name]=std::move(mapped);}}
            asset.parameters=std::move(values);
            const auto payload=Render::Assets::BuildResolvedMaterialPayload(asset);
            Pending item;item.source=materials[i].path;auto& request=item.request;
            request.sourceAssetId=materials[i].id;request.importerId="material-cooker";request.importerVersion=1;
            request.targetPlatform=options.targetPlatform;request.primarySubAssetKey="material:main";
            request.artifacts.emplace_back("material:main",A::ArtifactType::Material,"material","material",std::vector<uint8_t>(payload.begin(),payload.end()));
            for(const auto& pass:pending.front().request.artifacts)
                request.dependencies.push_back({A::AssetDependencyKind::ImportedArtifact,options.shaderId.ToString(),pass.subAssetKey});
            for(const auto& manifest:result.manifests)
                request.dependencies.push_back({A::AssetDependencyKind::ImportedArtifact,manifest.sourceAssetId.ToString(),manifest.primarySubAssetKey});
            pending.push_back(std::move(item));
        }
        for(const auto& item:pending)
        {
            auto committed=writer.WriteAndCommit(item.request,nullptr);
            if(!committed.committed){result.error="Material artifact publication failed";return result;}
            database.UpsertManifest(committed.manifest,item.source,A::ArtifactRecordStatus::UpToDate);
            result.manifests.push_back(std::move(committed.manifest));
        }
        result.snapshotId=closure.Id();
        auto id=result.snapshotId.substr(result.snapshotId.find(':')+1);
        if(!closure.Save(compile.generatedPath.parent_path()/"MaterialPublications"/(id+".json"),result.error))return result;
        if(!database.Save(databasePath)){result.error=database.GetLastError();return result;}
        result.databasePath=databasePath;return result;
    }
    catch(const std::exception& ex){result.error=ex.what();return result;}
}
}
