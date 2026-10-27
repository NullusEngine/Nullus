#include "ParticleSpriteMaterial.h"

#include <Core/ResourceManagement/MaterialManager.h>
#include <Core/ResourceManagement/MeshManager.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ResourceManagement/TextureManager.h>
#include <Core/ServiceLocator.h>
#include <Core/Assets/NativeArtifactContainer.h>
#include <algorithm>
#include <cmath>
#include <Math/Vector2.h>
#include <Math/Vector3.h>
#include <Math/Vector4.h>
#include <mutex>
#include <Rendering/Resources/Shader.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Resources/MaterialParameterBlock.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Settings/EGraphicsBackend.h>
#include "Engine/Serialize/ObjectReferenceResolver.h"

namespace NLS::Engine::Rendering
{
namespace
{
bool CaptureDomainMaterial(const Render::Resources::Material& material, const VFX::SpriteSettings& settings,
    const std::unordered_map<std::string,VFX::Value>& parameters, VFX::SpriteMaterial& captured, std::string& error)
{
    namespace rhi = Render::RHI;
    namespace resources = Render::Resources;
    const auto* shader = material.ResolveShaderForLightMode("ParticleSprite");
    if (!shader || shader->GetShaderLabLightMode() != "ParticleSprite")
    { error = "Selected material has no ParticleSprite shader pass"; return false; }
    const auto keywords = material.GetShaderLabKeywords().Hash();
    const auto backend=Render::Context::TryGetLocatedActiveGraphicsBackend();
    const auto target=backend && (*backend==Render::Settings::EGraphicsBackend::VULKAN || *backend==Render::Settings::EGraphicsBackend::METAL)
        ?Render::ShaderCompiler::ShaderTargetPlatform::SPIRV:Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    const auto reflection = shader->HasVariantReflections()?shader->GetReflectionSnapshot(target,keywords):shader->GetReflectionSnapshot();
    if(!reflection){error="Particle material active variant has no reflected binding layout";return false;}
    if (!resources::ValidateParticleSpriteReflection(*reflection,error)) return false;
    const auto identity = std::to_string(shader->GetInstanceId())+":"+std::to_string(shader->GetGeneration())+":"+std::to_string(keywords)+":"+std::to_string(static_cast<int>(target));
    static std::mutex cacheMutex;
    static std::unordered_map<std::string,std::weak_ptr<const VFX::SpriteMaterialProgram>> programs;
    {
        std::lock_guard lock(cacheMutex);
        std::erase_if(programs,[](const auto& item) { return item.second.expired(); });
        captured.program = programs[identity].lock();
        if (!captured.program)
        {
            auto program = std::make_shared<VFX::SpriteMaterialProgram>(); program->identity = identity;
            program->layout.debugName = "ParticleMaterial:"+material.path;
            for (const auto& buffer : reflection->constantBuffers)
                if (buffer.bindingSpace == rhi::BindingPointMap::kMaterialBindingSpace)
                    program->layout.entries.push_back({buffer.name,rhi::BindingType::UniformBuffer,rhi::BindingPointMap::kMaterialDescriptorSet,
                        buffer.bindingIndex,1,rhi::ShaderStageMask::Fragment,rhi::BindingPointMap::kMaterialBindingSpace,0});
            for (const auto& property : reflection->properties)
                if (property.bindingSpace == rhi::BindingPointMap::kMaterialBindingSpace &&
                    (property.kind == resources::ShaderResourceKind::SampledTexture || property.kind == resources::ShaderResourceKind::Sampler))
                    program->layout.entries.push_back({property.name,property.kind == resources::ShaderResourceKind::Sampler ? rhi::BindingType::Sampler : rhi::BindingType::Texture,
                        rhi::BindingPointMap::kMaterialDescriptorSet,property.bindingIndex,1,rhi::ShaderStageMask::Fragment,rhi::BindingPointMap::kMaterialBindingSpace,0});
            for (const auto& artifact : shader->GetCompiledArtifacts())
            {
                if (artifact.stage != Render::ShaderCompiler::ShaderStage::Pixel || artifact.keywordHash != keywords ||
                    artifact.output.bytecode.empty() || artifact.entryPoint != "ParticleMaterialPS") continue;
                rhi::RHIShaderModuleDesc module; module.stage = rhi::ShaderStage::Fragment; module.entryPoint = artifact.entryPoint;
                module.bytecode = artifact.output.bytecode;
                module.targetBackend = artifact.targetPlatform == Render::ShaderCompiler::ShaderTargetPlatform::DXIL ? rhi::NativeBackendType::DX12 :
                    artifact.targetPlatform == Render::ShaderCompiler::ShaderTargetPlatform::SPIRV ? rhi::NativeBackendType::Vulkan : rhi::NativeBackendType::None;
                if (module.targetBackend == rhi::NativeBackendType::None) continue;
                program->fragments.push_back(module);
                if (module.targetBackend == rhi::NativeBackendType::Vulkan) { module.targetBackend = rhi::NativeBackendType::Metal; program->fragments.push_back(std::move(module)); }
            }
            if (program->fragments.empty()) { error = "Particle material has no compiled fragment for its active keywords"; return false; }
            captured.program = program; programs[identity] = program;
        }
    }
    auto values = material.GetParameterBlock();
    for (const auto& binding : settings.materialParameters)
    {
        const auto property = std::find_if(reflection->properties.begin(),reflection->properties.end(),[&](const auto& property)
        { return property.name == binding.materialParameter && property.bindingSpace == 2 && property.kind == resources::ShaderResourceKind::Value; });
        const auto expected = static_cast<resources::UniformType>(static_cast<uint32_t>(resources::UniformType::UNIFORM_FLOAT)+static_cast<uint32_t>(binding.type));
        if (property == reflection->properties.end() || property->type != expected || property->arraySize != 1)
        { error = "Material parameter binding has a missing or incompatible target: "+binding.materialParameter; return false; }
        auto value = binding.defaultValue;
        if (const auto found = parameters.find(binding.systemParameter); found != parameters.end()) value = found->second;
        if (!std::all_of(value.begin(),value.end(),[](float v) { return std::isfinite(v); }))
        { error = "Material parameter values must be finite"; return false; }
        switch (binding.type)
        {
        case VFX::ValueType::Float: values.Set(binding.materialParameter,value[0]); break;
        case VFX::ValueType::Float2: values.Set(binding.materialParameter,Maths::Vector2(value[0],value[1])); break;
        case VFX::ValueType::Float3: values.Set(binding.materialParameter,Maths::Vector3(value[0],value[1],value[2])); break;
        case VFX::ValueType::Float4: values.Set(binding.materialParameter,Maths::Vector4(value[0],value[1],value[2],value[3])); break;
        }
    }
    for (const auto& buffer : reflection->constantBuffers)
        if (buffer.bindingSpace == 2)
        {
            VFX::SpriteMaterialConstant constant; constant.binding = buffer.bindingIndex;
            if (!resources::CaptureMaterialConstantBuffer(buffer,values,constant.bytes,error)) return false;
            captured.constants.push_back(std::move(constant));
        }
    for (const auto& property : reflection->properties)
    {
        if (property.bindingSpace != 2) continue;
        if (property.kind == resources::ShaderResourceKind::Sampler)
        {
            VFX::SpriteMaterialSampler sampler; sampler.binding = property.bindingIndex;
            if (const auto* override = material.GetSamplerOverride(property.name)) sampler.sampler = *override;
            captured.samplers.push_back(sampler);
        }
        if (property.kind != resources::ShaderResourceKind::SampledTexture) continue;
        VFX::SpriteMaterialTexture texture; texture.binding = property.bindingIndex;
        const auto path = material.GetTextureResourcePath(property.name);
        if (!path.empty())
        {
            if (!Core::ServiceLocator::Contains<Core::ResourceManagement::TextureManager>())
            { error = "Particle material texture manager is unavailable"; return false; }
            const auto* loaded = Core::ServiceLocator::Get<Core::ResourceManagement::TextureManager>().GetArtifactResource(path);
            if (!loaded || !(texture.texture = loaded->GetExplicitRHITextureHandle()))
            { error = "Particle material texture artifact is unavailable: "+property.name; return false; }
        }
        else if (const auto* value = values.TryGet(property.name))
            if (const auto* pointer = std::any_cast<resources::Texture2D*>(value); pointer && *pointer)
                texture.texture = (*pointer)->GetExplicitRHITextureHandle();
        captured.textures.push_back(std::move(texture));
    }
    return true;
}
bool Resolve(const VFX::AssetReference& reference, Core::Assets::ArtifactType type, std::string& path, std::string& error)
{
    const auto guid = Guid::TryParse(reference.guid);
    if (!guid || !guid->IsValid() || !reference.localId) { error = "Invalid particle asset reference"; return false; }
    const auto* entry = Serialize::ResolveRuntimeAssetReferenceEntry(Serialize::ObjectIdentifier::Asset(Serialize::AssetId(*guid),reference.localId));
    if (!entry || entry->artifactType != type || Core::Assets::TryMakePortableContentArtifactPath(entry->artifactPath).empty())
    { error = "Particle asset reference does not resolve to a typed imported artifact"; return false; }
    auto artifact = std::filesystem::path(entry->artifactPath);
    if (artifact.is_relative())
    {
        auto assets = std::filesystem::path(Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());
        if (assets.filename().empty()) assets = assets.parent_path();
        if (assets.empty()) { error = "Particle material project root is unavailable"; return false; }
        artifact = assets.parent_path()/artifact;
    }
    path = artifact.lexically_normal().string(); return true;
}
}

bool CaptureParticleMesh(const VFX::MeshSettings& settings, std::vector<std::shared_ptr<const VFX::MeshRenderGeometry>>& result,
    std::string& error, const SpriteAssetResolver& resolver)
{
    if (!VFX::ValidateMeshSettings(settings,error)) return false;
    if (settings.meshes.empty()) { error = "Mesh renderer requires at least one mesh slot"; return false; }
    if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>())
    { error = "Particle mesh manager is unavailable"; return false; }
    auto& manager = Core::ServiceLocator::Get<Core::ResourceManagement::MeshManager>();
    std::vector<std::shared_ptr<const VFX::MeshRenderGeometry>> captured;
    captured.reserve(settings.meshes.size());
    for (std::size_t slot = 0; slot < settings.meshes.size(); ++slot)
    {
        std::string path;
        if (!(resolver ? resolver(settings.meshes[slot],Core::Assets::ArtifactType::Mesh,path,error) : Resolve(settings.meshes[slot],Core::Assets::ArtifactType::Mesh,path,error)))
            return false;
        auto* mesh = manager.FindRegisteredMeshByResolvedArtifactPath(path);
        if (!mesh) mesh = manager.GetResource(path,true);
        if (!mesh || !mesh->GetIndexCount()) { error = "Particle mesh artifact requires indexed geometry"; return false; }
        auto geometry = std::make_shared<VFX::MeshRenderGeometry>();
        const auto first = mesh->GetMinLOD();
        const auto count = mesh->GetLODCount();
        if (!count || first >= count) { error = "Particle mesh LOD range is invalid"; return false; }
        // LOD0 holds the reference center; the union radius stays conservative so
        // per-view LOD switches never invalidate the emitted bounds.
        const auto* referenceMesh = mesh->GetLODMesh(first);
        if (!referenceMesh) { error = "Particle mesh reference LOD is unavailable"; return false; }
        const auto& reference = referenceMesh->GetBoundingSphere();
        VFX::Value center{reference.position.x,reference.position.y,reference.position.z,0};
        geometry->bounds = center;
        geometry->firstLod = first;
        for (uint32_t lod = first; lod < count; ++lod)
        {
            auto* level = mesh->GetLODMesh(lod);
            if (!level || !level->GetRHIMesh() || !level->GetIndexCount() || !level->GetVertexCount())
            { error = "Particle mesh LOD requires indexed geometry"; return false; }
            VFX::MeshRenderGeometry::Level capturedLevel;
            capturedLevel.mesh = level->GetRHIMesh();
            capturedLevel.screenSize = mesh->GetLODScreenSize(lod);
            geometry->levels.push_back(std::move(capturedLevel));
            const auto& sphere = level->GetBoundingSphere();
            const VFX::Value levelCenter{sphere.position.x,sphere.position.y,sphere.position.z,0};
            float distance = 0;
            for (size_t component = 0; component < 3; ++component)
                distance += (levelCenter[component]-center[component])*(levelCenter[component]-center[component]);
            geometry->bounds[3] = (std::max)(geometry->bounds[3],std::sqrt(distance)+sphere.radius);
        }
        if (!std::all_of(geometry->bounds.begin(),geometry->bounds.end(),[](float value) { return std::isfinite(value); }) || geometry->bounds[3] < 0)
        { error = "Particle mesh has invalid bounds"; return false; }
        captured.push_back(std::move(geometry));
    }
    result = std::move(captured); error.clear(); return true;
}

bool CaptureParticleSpriteMaterial(const VFX::SpriteSettings& settings, VFX::SpriteMaterial& result, std::string& error,
    const SpriteAssetResolver& resolver, const std::unordered_map<std::string,VFX::Value>& parameters)
{
    if (!VFX::ValidateSpriteSettings(settings,error)) return false;
    VFX::SpriteMaterial captured;
    captured.hasDynamicDefaults = true;
    for (size_t i = 0; i < settings.dynamicParameters.size(); ++i)
    {
        const auto& binding = settings.dynamicParameters[i]; captured.dynamicDefaults[i] = binding.defaultValue;
        if (const auto found = parameters.find(binding.fallbackParameter); !binding.fallbackParameter.empty() && found != parameters.end())
            captured.dynamicDefaults[i] = found->second;
        if (!std::all_of(captured.dynamicDefaults[i].begin(),captured.dynamicDefaults[i].end(),[](float value) { return std::isfinite(value); }))
        { error = "Dynamic material parameter values must be finite"; return false; }
    }
    std::string texturePath, textureName = "u_DiffuseMap";
    const auto resolve = [&](const VFX::AssetReference& ref, Core::Assets::ArtifactType type, std::string& path)
    { return resolver ? resolver(ref,type,path,error) : Resolve(ref,type,path,error); };
    if (!settings.material.Empty())
    {
        std::string path;
        if (!resolve(settings.material,Core::Assets::ArtifactType::Material,path)) return false;
        if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
        { error = "Particle material manager is unavailable"; return false; }
        auto& manager = Core::ServiceLocator::Get<Core::ResourceManagement::MaterialManager>();
        auto* material = manager.FindRegisteredMaterialByEquivalentArtifactPath(path);
        if (!material && settings.useMaterialShader)
        {
            Render::Resources::Loaders::MaterialLoader::LoadOptions options; options.loadMissingTextures = false;
            const auto payload = Core::Assets::ReadNativeArtifactPayloadTextFromFile(path,Core::Assets::ArtifactType::Material,1);
            if (!payload) { error = "Particle material native artifact could not be read"; return false; }
            for (const auto& dependency : payload->metadata.dependencies)
            {
                if (dependency.kind != Core::Assets::AssetDependencyKind::ImportedArtifact) continue;
                const auto guid = Guid::TryParse(dependency.value); if (!guid) continue;
                std::string artifact,errorMessage;
                const VFX::AssetReference reference{dependency.value,Serialize::MakeLocalIdentifierInFile(*guid,dependency.hashOrVersion)};
                if (resolver ? resolver(reference,Core::Assets::ArtifactType::Shader,artifact,errorMessage) : Resolve(reference,Core::Assets::ArtifactType::Shader,artifact,errorMessage))
                    options.shaderPassArtifactPaths.push_back(std::move(artifact));
            }
            if (options.shaderPassArtifactPaths.empty())
            { error = "Particle material needs imported shader dependencies; reimport its .mat asset"; return false; }
            material = manager.CreateResource(path,options);
            if (material) manager.RegisterResource(path,material);
        }
        else if (!material) material = manager.LoadArtifactWithoutTextures(path);
        if (!material) { error = "Particle material artifact could not be loaded"; return false; }
        if (settings.useMaterialShader)
        {
            if (!CaptureDomainMaterial(*material,settings,parameters,captured,error)) return false;
        }
        else
        {
        const auto& parameters = material->GetParameterBlock();
        for (const auto* name : {"u_Diffuse","u_Albedo","u_BaseColor","_BaseColor","_Color"})
            if (const auto* value = parameters.TryGet(name))
                if (const auto* color = std::any_cast<Maths::Vector4>(value))
                { captured.tint = {color->x,color->y,color->z,color->w}; break; }
        if (const auto* value = parameters.TryGet("u_TextureTiling"))
            if (const auto* tiling = std::any_cast<Maths::Vector2>(value)) { captured.uvTransform[0] = tiling->x; captured.uvTransform[1] = tiling->y; }
        if (const auto* value = parameters.TryGet("u_TextureOffset"))
            if (const auto* offset = std::any_cast<Maths::Vector2>(value)) { captured.uvTransform[2] = offset->x; captured.uvTransform[3] = offset->y; }
        for (const auto* name : {"_BaseMap_ST","_MainTex_ST"})
            if (const auto* value = parameters.TryGet(name))
                if (const auto* transform = std::any_cast<Maths::Vector4>(value))
                { captured.uvTransform = {transform->x,transform->y,transform->z,transform->w}; break; }
        if (const auto* value = parameters.TryGet("u_EmissiveIntensity"))
            if (const auto* emissive = std::any_cast<float>(value)) captured.emissive = *emissive;
        for (const auto* name : {"u_DiffuseMap","u_AlbedoMap","u_BaseColorMap","_BaseMap","_MainTex"})
            if (auto path = material->GetTextureResourcePath(name); !path.empty())
            { texturePath = std::move(path); textureName = name; break; }
        if (const auto* sampler = material->GetSamplerOverride(textureName)) captured.sampler = *sampler;
        }
    }
    if (!settings.texture.Empty() && !resolve(settings.texture,Core::Assets::ArtifactType::Texture,texturePath)) return false;
    if (!texturePath.empty())
    {
        if (!Core::ServiceLocator::Contains<Core::ResourceManagement::TextureManager>())
        { error = "Particle texture manager is unavailable"; return false; }
        auto* texture = Core::ServiceLocator::Get<Core::ResourceManagement::TextureManager>().GetArtifactResource(texturePath);
        if (!texture || !(captured.texture = texture->GetExplicitRHITextureHandle()))
        { error = "Particle texture artifact is unavailable on the rendering device"; return false; }
    }
    result = std::move(captured); error.clear(); return true;
}
}
