#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <GLFW/glfw3.h>
#include <Guid.h>
#include <Core/ServiceLocator.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ResourceManagement/MaterialManager.h>
#include <Core/ResourceManagement/TextureManager.h>
#include <Core/ResourceManagement/MeshManager.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetPath.h"
#include <Engine/Components/ParticleSystemComponent.h>
#include <Engine/Components/MeshFilter.h>
#include <Engine/Components/MeshRenderer.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Rendering/ForwardSceneRenderer.h>
#include <Engine/Rendering/DeferredSceneRenderer.h>
#include <Rendering/Context/Driver.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Context/RenderCommandPipe.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h>
#include <Rendering/Settings/DriverSettings.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Resources/Loaders/TextureLoader.h>
#include <VFXCompiler/VfxCompiler.h>
#include <VFXCompiler/VfxShaderCompiler.h>

static void ValidateParticleScenes(bool vulkan, bool spriteMaterials = false, bool domainMaterial = false, bool meshRenderer = false)
{
    using namespace NLS;
    namespace context = Render::Context;
    namespace rhi = Render::RHI;
    struct Window
    {
        GLFWwindow* handle = nullptr;
        ~Window() { if (handle) { glfwDestroyWindow(handle); glfwTerminate(); } }
    } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP() << "Vulkan WSI initialization unavailable";
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle = glfwCreateWindow(128,128,"VFX scenes",nullptr,nullptr);
        if (!window.handle) { glfwTerminate(); GTEST_SKIP() << "Vulkan WSI window unavailable"; }
    }
    Render::Settings::DriverSettings settings;
    settings.debugMode = false;
    settings.graphicsBackend = vulkan ? Render::Settings::EGraphicsBackend::VULKAN : Render::Settings::EGraphicsBackend::DX12;
    settings.platformWindow = window.handle;
    // Device creation itself uses the serialized submit lane for descriptor heaps.
    auto& executor = context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another fixture owns the RHI executor";
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { context::RhiCommandListExecutor& executor; ~Shutdown() { executor.Shutdown(); } } shutdown{executor};
    auto device = Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "Requested device unavailable";
    const auto validationBefore = Render::Backend::GetVulkanValidationErrorCountForTesting();
    Render::Settings::DriverSettings nullSettings;
    nullSettings.graphicsBackend = Render::Settings::EGraphicsBackend::NONE;
    context::Driver driver(nullSettings);
    context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    context::DriverTestAccess::SetExplicitDevice(driver,device);
    context::RhiCommandListExecutor::Get().Initialize(context::RhiThreadMode::None,context::RhiCommandListExecutor::CreateDeviceTargets(device));
    struct DriverScope
    {
        context::Driver* previous = nullptr;
        explicit DriverScope(context::Driver& current)
        {
            if (Core::ServiceLocator::Contains<context::Driver>()) previous = &Core::ServiceLocator::Get<context::Driver>();
            Core::ServiceLocator::Provide(current);
        }
        ~DriverScope() { if (previous) Core::ServiceLocator::Provide(*previous); else Core::ServiceLocator::Remove<context::Driver>(); }
    } scope(driver);
    auto asset = VFX::CreateDefaultSystem();
    asset.emitters.push_back(VFX::CreateDefaultSystem().emitters[0]);
    asset.emitters[1].target = VFX::SimulationTarget::GPU;
    for (size_t index = 0; index < asset.emitters.size(); ++index)
    {
        auto& emitter = asset.emitters[index]; emitter.spawn.nodes.clear(); emitter.spawnRate = 0; emitter.bursts = {{0,1}};
        emitter.sortByViewDepth = true;
        emitter.attributes[0].defaultValue = {index ? 0.55f : -0.55f,0,0,0};
        emitter.attributes[1].defaultValue = {};
        emitter.attributes[2].defaultValue = index ? VFX::Value{0,1,0,1} : VFX::Value{1,0,0,1};
        emitter.attributes[3].defaultValue = {0.8f,0,0,0};
    }
    asset.emitters[1].initialCapacity = 1;
    auto stage = VFX::CreateSimulationStage("Scene size"); stage.iterations = 2;
    const auto size = VFX::AddNode(stage.graph,VFX::Operation::Constant);
    VFX::FindNode(stage.graph,size)->value = {0.8f,0,0,0};
    const auto sizeOutput = VFX::AddNode(stage.graph,VFX::Operation::WriteAttribute);
    VFX::FindNode(stage.graph,sizeOutput)->name = "Particles.Size"; VFX::FindNode(stage.graph,sizeOutput)->inputs = {size};
    asset.emitters[1].simulationStages.push_back(stage);
    if (spriteMaterials)
    {
        asset.emitters[0].sprite.blend = VFX::SpriteBlendMode::Masked;
        asset.emitters[1].sprite.softDistance = 0.1f;
    }
    const auto root = std::filesystem::temp_directory_path()/("NullusVfxScene_"+Guid::New().ToString());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{root};
    struct AssetPathsScope
    {
        std::string previous = Core::ResourceManagement::ShaderManager::ProjectAssetsRoot();
        explicit AssetPathsScope(const std::filesystem::path& project)
        {
            std::filesystem::create_directories(project/"Assets");
            Core::ResourceManagement::ShaderManager::ProvideAssetPaths((project/"Assets").string(),
                (std::filesystem::path(NLS_ROOT_DIR)/"App"/"Assets"/"Engine").string());
        }
        ~AssetPathsScope()
        {
            Core::ResourceManagement::ShaderManager::ProvideAssetPaths(previous,
                (std::filesystem::path(NLS_ROOT_DIR)/"App"/"Assets"/"Engine").string());
        }
    } assetPaths(root);
    struct MaterialServices
    {
        Core::ResourceManagement::ShaderManager shaders;
        Core::ResourceManagement::MaterialManager materials;
        Core::ResourceManagement::TextureManager textures;
        Core::ResourceManagement::MeshManager meshes;
        std::unique_ptr<Engine::Assets::RuntimeAssetDatabase> assets;
        Core::ResourceManagement::ShaderManager* previousShaders = nullptr;
        Core::ResourceManagement::MaterialManager* previousMaterials = nullptr;
        Core::ResourceManagement::TextureManager* previousTextures = nullptr;
        Core::ResourceManagement::MeshManager* previousMeshes = nullptr;
        Engine::Assets::RuntimeAssetDatabase* previousAssets = nullptr;
        bool enabled = false;
        void Provide()
        {
            if (Core::ServiceLocator::Contains<Core::ResourceManagement::ShaderManager>()) previousShaders = &Core::ServiceLocator::Get<Core::ResourceManagement::ShaderManager>();
            if (Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>()) previousMaterials = &Core::ServiceLocator::Get<Core::ResourceManagement::MaterialManager>();
            if (Core::ServiceLocator::Contains<Core::ResourceManagement::TextureManager>()) previousTextures = &Core::ServiceLocator::Get<Core::ResourceManagement::TextureManager>();
            if (Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>()) previousMeshes = &Core::ServiceLocator::Get<Core::ResourceManagement::MeshManager>();
            if (Core::ServiceLocator::Contains<Engine::Assets::RuntimeAssetDatabase>()) previousAssets = &Core::ServiceLocator::Get<Engine::Assets::RuntimeAssetDatabase>();
            Core::ServiceLocator::Provide(shaders); Core::ServiceLocator::Provide(materials); Core::ServiceLocator::Provide(textures); Core::ServiceLocator::Provide(meshes); Core::ServiceLocator::Provide(*assets); enabled = true;
        }
        ~MaterialServices()
        {
            if (!enabled) return;
            materials.UnloadResources(); textures.UnloadResources(); shaders.UnloadResources(); meshes.UnloadResources();
            if (previousShaders) Core::ServiceLocator::Provide(*previousShaders); else Core::ServiceLocator::Remove<Core::ResourceManagement::ShaderManager>();
            if (previousMaterials) Core::ServiceLocator::Provide(*previousMaterials); else Core::ServiceLocator::Remove<Core::ResourceManagement::MaterialManager>();
            if (previousTextures) Core::ServiceLocator::Provide(*previousTextures); else Core::ServiceLocator::Remove<Core::ResourceManagement::TextureManager>();
            if (previousMeshes) Core::ServiceLocator::Provide(*previousMeshes); else Core::ServiceLocator::Remove<Core::ResourceManagement::MeshManager>();
            if (previousAssets) Core::ServiceLocator::Provide(*previousAssets); else Core::ServiceLocator::Remove<Engine::Assets::RuntimeAssetDatabase>();
        }
    } materialServices;
    if (domainMaterial)
    {
        const auto templates = std::filesystem::path(NLS_ROOT_DIR)/"Templates/VFX";
        for (const auto* file : {"ParticleMaterial.shader","ParticleMaterial.shader.meta","ParticleMaterial.mat","ParticleMaterial.mat.meta"})
            std::filesystem::copy_file(templates/file,root/"Assets"/file);
        NLS::Editor::Assets::AssetDatabaseFacade database(NLS::Editor::Assets::MakeProjectEditorAssetRoots(root)); ASSERT_TRUE(database.Refresh());
        if (meshRenderer)
        {
            std::filesystem::copy_file(templates/"Meshes/ParticleShard.gltf",root/"Assets/ParticleShard.gltf");
            std::filesystem::copy_file(templates/"Meshes/ParticleShard.gltf.meta",root/"Assets/ParticleShard.gltf.meta");
            ASSERT_TRUE(database.Refresh()); ASSERT_TRUE(database.ImportAsset("Assets/ParticleShard.gltf"));
        }
        for (const auto* path : {"Assets/ParticleMaterial.shader","Assets/ParticleMaterial.mat"})
        {
            const bool imported = database.ImportAsset(path);
            if (!imported) for (const auto& diagnostic : database.GetDiagnostics()) ADD_FAILURE() << diagnostic.message;
            ASSERT_TRUE(imported);
        }
        const auto material = database.LoadMainAssetAtPath("Assets/ParticleMaterial.mat"); ASSERT_TRUE(material);
        Engine::Assets::RuntimeManifestBuilder builder;
        VFX::AssetReference meshReference;
        if (meshRenderer)
        {
            const auto meshManifest = database.GetArtifactManifestForAssetPath("Assets/ParticleShard.gltf"); ASSERT_TRUE(meshManifest);
            builder.AddArtifactManifest(*meshManifest);
            for (const auto& record : database.LoadAllAssetsAtPath("Assets/ParticleShard.gltf"))
                if (record.artifactType == Core::Assets::ArtifactType::Mesh)
                {
                    meshReference = {record.assetId.ToString(),Engine::Serialize::MakeLocalIdentifierInFile(record.assetId.GetGuid(),record.subAssetKey)};
                    break;
                }
            ASSERT_FALSE(meshReference.Empty());
            for (size_t emitterIndex = 0; emitterIndex < asset.emitters.size(); ++emitterIndex)
            {
                auto& emitter = asset.emitters[emitterIndex];
                emitter.renderer = VFX::RendererType::Mesh; emitter.mesh.meshes = {meshReference,meshReference};
                emitter.attributes.push_back({"Particles.MeshIndex",VFX::ValueType::Float,{static_cast<float>(emitterIndex),0,0,0}});
                emitter.mesh.defaultScale = {0.8f,0.8f,0.8f,0};
            }
        }
        for (const auto* path : {"Assets/ParticleMaterial.shader","Assets/ParticleMaterial.mat"})
        { const auto manifest = database.GetArtifactManifestForAssetPath(path); ASSERT_TRUE(manifest); builder.AddArtifactManifest(*manifest); }
        auto roots = std::vector<Engine::Assets::RuntimeAssetRef>{{material->assetId,material->subAssetKey}};
        if (meshRenderer)
            for (const auto& record : database.LoadAllAssetsAtPath("Assets/ParticleShard.gltf"))
                if (record.artifactType == Core::Assets::ArtifactType::Mesh) roots.push_back({record.assetId,record.subAssetKey});
        const auto manifest = builder.Build(roots,"editor"); ASSERT_FALSE(manifest.diagnostics.HasErrors());
        materialServices.assets = std::make_unique<Engine::Assets::RuntimeAssetDatabase>(manifest.manifest); materialServices.Provide();
        for (auto& emitter : asset.emitters)
        {
            emitter.sprite.useMaterialShader = true;
            emitter.sprite.material = {material->assetId.ToString(),Engine::Serialize::MakeLocalIdentifierInFile(material->assetId.GetGuid(),material->subAssetKey)};
            emitter.sprite.materialParameters = {{"_Tint","User.Tint",VFX::ValueType::Float4,{1,1,1,1}}};
        }
        std::filesystem::remove(root/"Assets/ParticleMaterial.shader"); std::filesystem::remove(root/"Assets/ParticleMaterial.mat");
        std::filesystem::remove(root/"Assets/ParticleMaterial.shader.meta"); std::filesystem::remove(root/"Assets/ParticleMaterial.mat.meta");
        if (meshRenderer) { std::filesystem::remove(root/"Assets/ParticleShard.gltf"); std::filesystem::remove(root/"Assets/ParticleShard.gltf.meta"); }
        std::filesystem::rename(root/"Library/ArtifactDB",root/"Library/ArtifactDB.unavailable");
    }
    auto program = std::make_shared<VFX::CompiledSystem>();
    std::vector<VFX::Diagnostic> diagnostics;
    std::string error;
    ASSERT_TRUE(VFX::CompileSystem(asset,*program,diagnostics));
    const auto backend = device->GetAdapter()->GetBackendType();
    ASSERT_TRUE(VFX::CompilePlatformPrograms(*program,std::span(&backend,1),root,diagnostics)) << (diagnostics.empty() ? "" : diagnostics[0].message);
    ASSERT_TRUE(VFX::DeserializeCompiledSystem(VFX::SerializeCompiledSystem(*program,false),*program,error)) << error;
    for (const bool deferred : {false,true})
    {
        SCOPED_TRACE(deferred ? "Deferred" : "Forward");
        const auto destroyShader = [](Render::Resources::Shader* shader) { Render::Resources::Loaders::ShaderLoader::Destroy(shader); };
        std::unique_ptr<Render::Resources::Shader,decltype(destroyShader)> occluderShader(nullptr,destroyShader);
        std::unique_ptr<Render::Resources::Material> occluderMaterial;
        std::unique_ptr<Render::Resources::Mesh> occluderMesh;
        std::unique_ptr<Render::Resources::Texture2D> whiteTexture;
        Engine::SceneSystem::Scene scene;
        auto& object = scene.CreateGameObject("Mixed particles");
        object.GetTransform()->SetWorldPosition({0,0,4});
        auto* component = object.AddComponent<Engine::Components::ParticleSystemComponent>(); ASSERT_NE(component,nullptr);
        ASSERT_TRUE(component->SetCompiledSystem(program,error)) << error;
        if (domainMaterial) ASSERT_TRUE(component->SetVectorParameter("User.Tint",{1,1,1,1}));
        component->OnUpdate(1.0f/60);
        const auto publication = component->CaptureFrame();
        std::unique_ptr<Engine::Rendering::BaseSceneRenderer> renderer;
        if (deferred) renderer = std::make_unique<Engine::Rendering::DeferredSceneRenderer>(driver);
        else renderer = std::make_unique<Engine::Rendering::ForwardSceneRenderer>(driver);
        std::vector<context::RenderScenePackage> observed;
        context::DriverTestAccess::SetRenderSceneObserver(driver,[&](const auto&, const auto& package) { observed.push_back(package); });
        struct Drain
        {
            context::Driver& driver;
            ~Drain() { context::DriverTestAccess::DrainThreadedRendering(driver); context::DriverTestAccess::SetRenderSceneObserver(driver,{}); }
        } drain{driver};
        std::vector<std::vector<uint8_t>> images;
        for (uint32_t view = 0; view < 5; ++view)
        {
            if (view == 3)
            {
                // Enter the ordinary opaque/GBuffer path and verify that sprites
                // consume its depth instead of an unrelated external depth target.
                occluderShader.reset(Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
                    Render::Resources::Loaders::ShaderLoader::ResolveTrustedBuiltInEngineShaderPath("Unlit.hlsl")));
                ASSERT_NE(occluderShader,nullptr);
                occluderMaterial = std::make_unique<Render::Resources::Material>();
                occluderMaterial->SetShader(occluderShader.get()); occluderMaterial->SetBackfaceCulling(false);
                whiteTexture.reset(Render::Resources::Loaders::TextureLoader::CreatePixel(255,255,255,255)); ASSERT_NE(whiteTexture,nullptr);
                for (const auto* name : {"u_MaskMap","u_DiffuseMap","u_OpacityMap","u_AlbedoMap"})
                    occluderMaterial->Set<Render::Resources::Texture2D*>(name,whiteTexture.get());
                occluderMaterial->SetRawParameter("u_Diffuse",Maths::Vector4(0.3f,0.3f,0.3f,1));
                occluderMaterial->SetRawParameter("u_Albedo",Maths::Vector4(0.3f,0.3f,0.3f,1));
                std::vector<Render::Geometry::Vertex> vertices(4);
                const float xy[4][2] = {{-5,-5},{5,-5},{5,5},{-5,5}};
                for (size_t index = 0; index < vertices.size(); ++index)
                { vertices[index].position[0] = xy[index][0]; vertices[index].position[1] = xy[index][1]; vertices[index].normals[2] = -1; }
                occluderMesh = std::make_unique<Render::Resources::Mesh>(vertices,std::vector<uint32_t>{0,1,2,0,2,3,2,1,0,3,2,0},0u,
                    Render::Resources::MeshBufferUploadMode::CpuToGpu,Render::Geometry::BoundingSphere{{0,0,0},8});
                auto& wall = scene.CreateGameObject("Occluder"); wall.GetTransform()->SetWorldPosition({0,0,2});
                wall.AddComponent<Engine::Components::MeshFilter>()->SetMesh(occluderMesh.get());
                wall.AddComponent<Engine::Components::MeshRenderer>()->FillWithMaterial(*occluderMaterial);
                component->Restart(); component->OnUpdate(1.0f/60);
            }
            if (view == 4) object.GetTransform()->SetWorldPosition({1000,0,4});
            rhi::RHITextureDesc colorDesc; colorDesc.extent = {128,128,1}; colorDesc.format = rhi::TextureFormat::RGBA8;
            colorDesc.usage = rhi::TextureUsageFlags::ColorAttachment|rhi::TextureUsageFlags::CopySrc|rhi::TextureUsageFlags::Sampled;
            auto color = device->CreateTexture(colorDesc); ASSERT_NE(color,nullptr);
            auto depthDesc = colorDesc; depthDesc.format = rhi::TextureFormat::Depth24Stencil8; depthDesc.usage = rhi::TextureUsageFlags::DepthStencilAttachment;
            if (spriteMaterials) depthDesc.usage = depthDesc.usage|rhi::TextureUsageFlags::Sampled;
            auto depth = device->CreateTexture(depthDesc); ASSERT_NE(depth,nullptr);
            rhi::RHITextureViewDesc viewDesc; viewDesc.format = colorDesc.format;
            auto colorView = device->CreateTextureView(color,viewDesc); ASSERT_NE(colorView,nullptr);
            viewDesc.format = depthDesc.format; auto depthView = device->CreateTextureView(depth,viewDesc); ASSERT_NE(depthView,nullptr);
            Maths::Transform cameraTransform;
            Render::Entities::Camera camera(&cameraTransform);
            Render::Data::FrameDescriptor frame;
            frame.camera = &camera; frame.renderWidth = frame.renderHeight = 128; frame.clearColorOverride = Maths::Vector4(0,0,0,1);
            frame.outputColorTexture = color; frame.outputColorView = colorView; frame.outputDepthStencilTexture = depth; frame.outputDepthStencilView = depthView;
            Engine::Rendering::BaseSceneRenderer::SceneDescriptor sceneDescriptor{scene,std::nullopt,nullptr};
            sceneDescriptor.suppressLightGridCompute = true; sceneDescriptor.suppressHZBOcclusion = true;
            renderer->AddDescriptor(std::move(sceneDescriptor));
            renderer->BeginFrame(frame); renderer->DrawFrame(); renderer->EndFrame();
            // This mutation occurs after publication and must affect only the next view.
            if (view == 0)
            {
                object.GetTransform()->SetWorldPosition({0.5f,0,4});
                if (domainMaterial) ASSERT_TRUE(component->SetVectorParameter("User.Tint",{0.8f,0.8f,0.8f,1}));
            }
            if (view == 1) component->Stop(true);
            ASSERT_TRUE(context::DriverTestAccess::TryDrainThreadedRendering(driver));
            ASSERT_FALSE(context::RhiCommandListExecutor::Get().HasFailed()) << context::RhiCommandListExecutor::Get().GetFailureDiagnostic();
            std::vector<uint8_t> pixels(128*128*4);
            const auto read = device->BeginReadPixels(color,0,0,128,128,Render::Settings::EPixelDataFormat::RGBA,Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
            ASSERT_TRUE(read.Succeeded()) << read.message;
            if (read.completion) ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
            uint32_t red = 0, green = 0;
            for (size_t index = 0; index < pixels.size(); index += 4)
            { if (pixels[index] > 180 && pixels[index+1] < 50) ++red; if (pixels[index+1] > 180 && pixels[index] < 50) ++green; }
            if (view < 2) { EXPECT_GT(red,20u); EXPECT_GT(green,20u); }
            else { EXPECT_EQ(red,0u); EXPECT_EQ(green,0u); }
            images.push_back(std::move(pixels));
        }
        EXPECT_NE(images[0],images[1]);
        ASSERT_EQ(observed.size(),5u);
        EXPECT_GT(observed[3].opaqueDrawCount,0u);
        const auto opaque = std::find_if(observed[3].passCommandInputs.begin(),observed[3].passCommandInputs.end(),[](const auto& pass)
            { return pass.kind == context::RenderPassCommandKind::Opaque || pass.kind == context::RenderPassCommandKind::GBuffer; });
        ASSERT_NE(opaque,observed[3].passCommandInputs.end()); ASSERT_FALSE(opaque->recordedDrawCommands.empty());
        EXPECT_TRUE(opaque->recordedDrawCommands[0].pipeline->GetDesc().depthStencilState.depthWrite);
        const auto& occludedSprites = observed[3].passCommandInputs.back();
        EXPECT_FALSE(occludedSprites.clearDepth); EXPECT_EQ(occludedSprites.depthStencilAttachmentView,opaque->depthStencilAttachmentView);
        for (size_t index = 0; index < 2; ++index)
        {
            const auto& package = observed[index];
            const auto pass = std::find_if(package.passCommandInputs.begin(),package.passCommandInputs.end(),[](const auto& input) { return input.debugName == "VfxSceneSprites"; });
            ASSERT_NE(pass,package.passCommandInputs.end()); ASSERT_EQ(pass->recordedDrawCommands.size(),(spriteMaterials ? 1u : 2u)*(meshRenderer ? 2u : 1u));
            EXPECT_EQ(package.parallelDrawCommandBatches.size(),package.parallelCommandWorkUnits.size());
            EXPECT_TRUE(pass->recordedDrawCommands[0].resourceOwner); EXPECT_TRUE(pass->recordedDrawCommands.back().indirectArguments);
            if (meshRenderer) EXPECT_TRUE(pass->recordedDrawCommands[0].mesh);
            EXPECT_TRUE(pass->depthStencilAttachmentView);
            EXPECT_FALSE(pass->computeDispatchInputs.empty());
            if (domainMaterial)
                for (const auto& draw : pass->recordedDrawCommands)
                {
                    ASSERT_TRUE(draw.materialBindingSet);
                    const auto& entries = draw.materialBindingSet->GetDesc().entries;
                    const auto constant = std::find_if(entries.begin(),entries.end(),[](const auto& entry) { return entry.type == rhi::BindingType::UniformBuffer; });
                    ASSERT_NE(constant,entries.end()); float tint = 0;
                    rhi::RHIBufferReadbackDesc request; request.source = constant->buffer; request.sourceOffset = constant->bufferOffset; request.size = sizeof(tint); request.data = &tint;
                    const auto read = device->BeginReadBuffer(request); ASSERT_TRUE(read.Succeeded()) << read.message;
                    if (read.completion) ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
                    EXPECT_FLOAT_EQ(tint,index == 0 ? 1.0f : 0.8f);
                }
            if (spriteMaterials)
            {
                const auto solid = std::find_if(package.passCommandInputs.begin(),package.passCommandInputs.end(),[](const auto& input) { return input.debugName == "VfxSceneSolidSprites"; });
                ASSERT_NE(solid,package.passCommandInputs.end()); EXPECT_LT(solid,pass);
                EXPECT_TRUE(solid->writesDepthStencilAttachment);
                EXPECT_TRUE(solid->recordedDrawCommands[0].pipeline->GetDesc().depthStencilState.depthWrite);
                EXPECT_FALSE(pass->writesDepthStencilAttachment);
                const auto depthAccess = std::find_if(pass->textureResourceAccesses.begin(),pass->textureResourceAccesses.end(),[&](const auto& access) { return access.texture == pass->depthStencilAttachmentView->GetTexture(); });
                ASSERT_NE(depthAccess,pass->textureResourceAccesses.end());
                EXPECT_EQ(depthAccess->state,rhi::ResourceState::DepthRead|rhi::ResourceState::ShaderRead);
                const auto& entries = pass->recordedDrawCommands[0].passBindingSet->GetDesc().entries;
                const auto depthBinding = std::find_if(entries.begin(),entries.end(),[](const auto& entry) { return entry.binding == 3 && entry.type == rhi::BindingType::Texture; });
                ASSERT_NE(depthBinding,entries.end()); EXPECT_EQ(depthBinding->textureView->GetTexture(),depthAccess->texture);
                EXPECT_EQ(depthBinding->textureState,depthAccess->state);
            }
            for (size_t unitIndex = 0; unitIndex < package.parallelCommandWorkUnits.size(); ++unitIndex)
            {
                const auto& unit = package.parallelCommandWorkUnits[unitIndex]; EXPECT_EQ(unit.workUnitIndex,unitIndex);
                if (unit.sourcePassIndex != context::kInvalidParallelCommandSourcePassIndex)
                { ASSERT_LT(unit.sourcePassIndex,package.passCommandInputs.size()); EXPECT_EQ(unit.commandInput.debugName,package.passCommandInputs[unit.sourcePassIndex].debugName); }
                for (const auto& edge : unit.incomingDependencyEdges) { EXPECT_LT(edge.sourceWorkUnitIndex,edge.targetWorkUnitIndex); EXPECT_EQ(edge.targetWorkUnitIndex,unit.workUnitIndex); }
            }
        }
        EXPECT_DOUBLE_EQ(publication->age,component->CaptureFrame()->age);
        EXPECT_EQ(publication->cpu[0]->Count(),1u);
        // The captured packages retain all buffers after component removal.
        component->OnDestroy(); EXPECT_EQ(component->CaptureFrame(),nullptr);
        if (meshRenderer) materialServices.meshes.UnloadResources();
        EXPECT_TRUE(observed[0].passCommandInputs.back().recordedDrawCommands[0].resourceOwner);
        for (size_t view = 0; view < observed.size(); ++view)
            for (const auto& command : observed[view].passCommandInputs.back().recordedDrawCommands)
            {
                VFX::ParticleViewStatistics statistics;
                rhi::RHIBufferReadbackDesc request; request.source = command.indirectArguments;
                request.size = sizeof(statistics); request.data = &statistics;
                const auto read = device->BeginReadBuffer(request); ASSERT_TRUE(read.Succeeded()) << read.message;
                if (read.completion) ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
                EXPECT_EQ(statistics.particleCount,view == 2 ? 0u : 1u);
                EXPECT_EQ(statistics.visibleCount,view == 2 || view == 4 ? 0u : 1u);
                EXPECT_EQ(statistics.instanceCount,statistics.visibleCount);
                EXPECT_EQ(statistics.culledCount,view == 4 ? 1u : 0u);
                EXPECT_EQ(statistics.invalidCount,0u);
                if (meshRenderer)
                {
                    ASSERT_TRUE(command.mesh); EXPECT_EQ(statistics.vertexCount,command.mesh->GetIndexCount());
                    EXPECT_TRUE(command.mesh->GetVertexBuffer()); EXPECT_TRUE(command.mesh->GetIndexBuffer());
                }
            }
        std::shared_ptr<rhi::RHIBuffer> previousViewKeys;
        for (const auto& package : {&observed[0],&observed[1]})
            for (const auto& command : package->passCommandInputs.back().recordedDrawCommands)
            {
                const auto& entries = command.passBindingSet->GetDesc().entries;
                const auto keys = std::find_if(entries.begin(),entries.end(),[](const auto& entry)
                    { return entry.binding == 1 && entry.type == rhi::BindingType::StructuredBuffer; });
                ASSERT_NE(keys,entries.end());
                // Mesh slots in a view share sorted indices; different views own
                // independent index buffers even while consuming the same frame.
                if (!meshRenderer || &command == &package->passCommandInputs.back().recordedDrawCommands.front())
                    EXPECT_NE(keys->buffer,previousViewKeys);
                previousViewKeys = keys->buffer;
                std::array<std::array<uint32_t,4>,2> sorted{};
                rhi::RHIBufferReadbackDesc request; request.source = keys->buffer; request.size = sizeof(sorted); request.data = sorted.data();
                const auto read = device->BeginReadBuffer(request); ASSERT_TRUE(read.Succeeded()) << read.message;
                if (read.completion) ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
                EXPECT_EQ(sorted[0][3],0u); EXPECT_EQ(sorted[1][3],~0u);
            }
    }
    if (vulkan) EXPECT_EQ(Render::Backend::GetVulkanValidationErrorCountForTesting(),validationBefore);
}

TEST(VfxSceneTests, Dx12ForwardAndDeferredRenderMixedParticlesFromCookedAssets)
{
#if defined(_WIN32)
    ValidateParticleScenes(false);
#else
    GTEST_SKIP() << "DX12 validation requires Windows";
#endif
}

TEST(VfxSceneTests, VulkanForwardAndDeferredRenderMixedParticlesFromCookedAssets)
{
    ValidateParticleScenes(true);
}

TEST(VfxSceneTests, Dx12ForwardAndDeferredOrderMaskedSpritesBeforeSoftSprites)
{
#if defined(_WIN32)
    ValidateParticleScenes(false,true);
#else
    GTEST_SKIP() << "DX12 validation requires Windows";
#endif
}

TEST(VfxSceneTests, VulkanForwardAndDeferredOrderMaskedSpritesBeforeSoftSprites)
{
    ValidateParticleScenes(true,true);
}

TEST(VfxSceneTests, Dx12ParticleMaterialPassUsesFrozenUserParametersInForwardAndDeferred)
{
#if defined(_WIN32)
    ValidateParticleScenes(false,false,true);
#else
    GTEST_SKIP() << "DX12 validation requires Windows";
#endif
}

TEST(VfxSceneTests, VulkanParticleMaterialPassUsesFrozenUserParametersInForwardAndDeferred)
{
    ValidateParticleScenes(true,false,true);
}

TEST(VfxSceneTests, Dx12MeshParticlesUseImportedGeometryAndMaterialInForwardAndDeferred)
{
#if defined(_WIN32)
    ValidateParticleScenes(false,true,true,true);
#else
    GTEST_SKIP() << "DX12 requires Windows";
#endif
}
TEST(VfxSceneTests, VulkanMeshParticlesUseImportedGeometryAndMaterialInForwardAndDeferred)
{
    ValidateParticleScenes(true,true,true,true);
}

/**
 * Cross-emitter transparent ordering.
 *
 * Two translucent CPU emitters overlap on the centre pixel at different
 * depths. Emitters sort internally, so nothing inside either one can order
 * particles that belong to the other; only the pass order decides the
 * interleave. The nearer component is declared first, which is the order an
 * unsorted pass would use, so the composite exposes which one drew first:
 * correct far-to-near ordering leaves the nearer blue emitter with the larger
 * share of the pixel, while a near-first order leaves red dominant.
 */
static void ValidateTransparentEmitterOrdering(bool vulkan, bool gpuEmitters = false)
{
    using namespace NLS;
    namespace context = Render::Context;
    namespace rhi = Render::RHI;
    struct Window
    {
        GLFWwindow* handle = nullptr;
        bool initialized = false;
        ~Window()
        {
            if (handle) glfwDestroyWindow(handle);
            if (initialized) glfwTerminate();
        }
    } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP() << "Vulkan WSI initialization unavailable";
        window.initialized = true;
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle = glfwCreateWindow(128,128,"VFX emitter ordering",nullptr,nullptr);
        if (!window.handle) GTEST_SKIP() << "Vulkan WSI window unavailable";
    }
    Render::Settings::DriverSettings settings;
    settings.debugMode = false;
    settings.graphicsBackend = vulkan ? Render::Settings::EGraphicsBackend::VULKAN : Render::Settings::EGraphicsBackend::DX12;
    settings.platformWindow = window.handle;
    auto& executor = context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another fixture owns the RHI executor";
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { context::RhiCommandListExecutor& executor; ~Shutdown() { executor.Shutdown(); } } shutdown{executor};
    auto device = Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "Requested device unavailable";
    const auto validationBefore = Render::Backend::GetVulkanValidationErrorCountForTesting();
    Render::Settings::DriverSettings nullSettings;
    nullSettings.graphicsBackend = Render::Settings::EGraphicsBackend::NONE;
    context::Driver driver(nullSettings);
    context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    context::DriverTestAccess::SetExplicitDevice(driver,device);
    context::RhiCommandListExecutor::Get().Initialize(context::RhiThreadMode::None,context::RhiCommandListExecutor::CreateDeviceTargets(device));
    struct DriverScope
    {
        context::Driver* previous = nullptr;
        explicit DriverScope(context::Driver& current)
        {
            if (Core::ServiceLocator::Contains<context::Driver>()) previous = &Core::ServiceLocator::Get<context::Driver>();
            Core::ServiceLocator::Provide(current);
        }
        ~DriverScope() { if (previous) Core::ServiceLocator::Provide(*previous); else Core::ServiceLocator::Remove<context::Driver>(); }
    } scope(driver);

    const auto root = std::filesystem::temp_directory_path()/("NullusVfxOrder_"+Guid::New().ToString());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{root};
    struct AssetPathsScope
    {
        std::string previous = Core::ResourceManagement::ShaderManager::ProjectAssetsRoot();
        explicit AssetPathsScope(const std::filesystem::path& project)
        {
            std::filesystem::create_directories(project/"Assets");
            Core::ResourceManagement::ShaderManager::ProvideAssetPaths((project/"Assets").string(),
                (std::filesystem::path(NLS_ROOT_DIR)/"App"/"Assets"/"Engine").string());
        }
        ~AssetPathsScope()
        {
            Core::ResourceManagement::ShaderManager::ProvideAssetPaths(previous,
                (std::filesystem::path(NLS_ROOT_DIR)/"App"/"Assets"/"Engine").string());
        }
    } assetPaths(root);

    const auto makeEmitter = [](const uint32_t count,const VFX::Value color,const VFX::SimulationTarget target,
        const float localDepth)
    {
        auto system = VFX::CreateDefaultSystem("Emitter order");
        auto& emitter = system.emitters.front();
        emitter.target = target;
        emitter.spawn.nodes.clear(); emitter.update.nodes.clear();
        emitter.spawnRate = 0; emitter.duration = 1; emitter.looping = false; emitter.bursts = {{0,count}};
        emitter.capacity = 16; emitter.initialCapacity = target == VFX::SimulationTarget::GPU ? 4 : 0;
        emitter.sortByViewDepth = true; emitter.frustumCulling = false;
        emitter.attributes[0].defaultValue = {0,0,localDepth,0};
        emitter.attributes[1].defaultValue = {};
        emitter.attributes[2].defaultValue = color;
        emitter.attributes[3].defaultValue = {1.0f,0,0,0};
        emitter.sprite.circularMask = false; emitter.sprite.blend = VFX::SpriteBlendMode::Translucent;
        return emitter;
    };
    const auto backend = device->GetAdapter()->GetBackendType();
    std::vector<VFX::Diagnostic> diagnostics;
    std::string error;
    const auto compile = [&](const VFX::SystemAsset& asset) -> std::shared_ptr<VFX::CompiledSystem>
    {
        auto program = std::make_shared<VFX::CompiledSystem>();
        EXPECT_TRUE(VFX::CompileSystem(asset,*program,diagnostics))
            << (diagnostics.empty() ? std::string {} : diagnostics.front().message);
        EXPECT_TRUE(VFX::CompilePlatformPrograms(*program,std::span(&backend,1),root/"Shaders",diagnostics))
            << (diagnostics.empty() ? std::string {} : diagnostics.front().message);
        return program;
    };
    Engine::SceneSystem::Scene scene;
    Engine::Components::ParticleSystemComponent* particleComponent = nullptr;
    Engine::Components::ParticleSystemComponent* nearComponent = nullptr;
    Engine::Components::ParticleSystemComponent* farComponent = nullptr;
    if (gpuEmitters)
    {
        auto system = VFX::CreateDefaultSystem("GPU emitter order");
        // Same component and declaration order: near first, far second.
        system.emitters = {makeEmitter(1,{0,0,1,0.5f},VFX::SimulationTarget::GPU,4),
            makeEmitter(2,{1,0,0,0.5f},VFX::SimulationTarget::GPU,8)};
        const auto program = compile(system);
        ASSERT_TRUE(program->FindPlatform(backend));
        auto& object = scene.CreateGameObject("GPU particles");
        particleComponent = object.AddComponent<Engine::Components::ParticleSystemComponent>();
        ASSERT_NE(particleComponent,nullptr);
        ASSERT_TRUE(particleComponent->SetCompiledSystem(program,error)) << error;
    }
    else
    {
        auto nearSystem = VFX::CreateDefaultSystem("Near emitter");
        nearSystem.emitters = {makeEmitter(1,{0,0,1,0.5f},VFX::SimulationTarget::CPU,0)};
        auto farSystem = VFX::CreateDefaultSystem("Far emitter");
        farSystem.emitters = {makeEmitter(2,{1,0,0,0.5f},VFX::SimulationTarget::CPU,0)};
        const auto nearProgram = compile(nearSystem);
        const auto farProgram = compile(farSystem);
        ASSERT_TRUE(nearProgram->FindPlatform(backend));
        ASSERT_TRUE(farProgram->FindPlatform(backend));
        // The relation is the same in world space: near component first, far second.
        auto& nearObject = scene.CreateGameObject("Near particles");
        nearObject.GetTransform()->SetWorldPosition({0,0,4});
        nearComponent = nearObject.AddComponent<Engine::Components::ParticleSystemComponent>();
        ASSERT_NE(nearComponent,nullptr);
        ASSERT_TRUE(nearComponent->SetCompiledSystem(nearProgram,error)) << error;
        auto& farObject = scene.CreateGameObject("Far particles");
        farObject.GetTransform()->SetWorldPosition({0,0,8});
        farComponent = farObject.AddComponent<Engine::Components::ParticleSystemComponent>();
        ASSERT_NE(farComponent,nullptr);
        ASSERT_TRUE(farComponent->SetCompiledSystem(farProgram,error)) << error;
    }

    auto renderer = std::make_unique<Engine::Rendering::ForwardSceneRenderer>(driver);
    std::vector<context::RenderScenePackage> observed;
    context::DriverTestAccess::SetRenderSceneObserver(driver,[&](const auto&,const auto& package) { observed.push_back(package); });
    struct Drain
    {
        context::Driver& driver;
        ~Drain() { context::DriverTestAccess::DrainThreadedRendering(driver); context::DriverTestAccess::SetRenderSceneObserver(driver,{}); }
    } drain{driver};

    rhi::RHITextureDesc colorDesc; colorDesc.extent = {128,128,1}; colorDesc.format = rhi::TextureFormat::RGBA8;
    colorDesc.usage = rhi::TextureUsageFlags::ColorAttachment|rhi::TextureUsageFlags::CopySrc|rhi::TextureUsageFlags::Sampled;
    auto color = device->CreateTexture(colorDesc); ASSERT_NE(color,nullptr);
    auto depthDesc = colorDesc; depthDesc.format = rhi::TextureFormat::Depth24Stencil8;
    depthDesc.usage = rhi::TextureUsageFlags::DepthStencilAttachment;
    auto depth = device->CreateTexture(depthDesc); ASSERT_NE(depth,nullptr);
    rhi::RHITextureViewDesc viewDesc; viewDesc.format = colorDesc.format;
    auto colorView = device->CreateTextureView(color,viewDesc); ASSERT_NE(colorView,nullptr);
    viewDesc.format = depthDesc.format;
    auto depthView = device->CreateTextureView(depth,viewDesc); ASSERT_NE(depthView,nullptr);
    Maths::Transform cameraTransform;
    Render::Entities::Camera camera(&cameraTransform);
    Render::Data::FrameDescriptor frame;
    frame.camera = &camera; frame.renderWidth = frame.renderHeight = 128; frame.clearColorOverride = Maths::Vector4(0,0,0,1);
    frame.outputColorTexture = color; frame.outputColorView = colorView;
    frame.outputDepthStencilTexture = depth; frame.outputDepthStencilView = depthView;
    if (!gpuEmitters)
    {
        nearComponent->OnUpdate(1.0f/60); farComponent->OnUpdate(1.0f/60);
        ASSERT_EQ(nearComponent->CaptureFrame()->cpu[0]->Count(),1u);
        ASSERT_EQ(farComponent->CaptureFrame()->cpu[0]->Count(),2u);
    }
    // GPU ordering bounds are read back asynchronously. Several submitted
    // frames let the readback complete without inserting a CPU wait.
    const uint32_t renderedFrames = gpuEmitters ? 6u : 1u;
    for (uint32_t frameIndex = 0; frameIndex < renderedFrames; ++frameIndex)
    {
        if (gpuEmitters) particleComponent->OnUpdate(1.0f/60);
        Engine::Rendering::BaseSceneRenderer::SceneDescriptor sceneDescriptor{scene,std::nullopt,nullptr};
        sceneDescriptor.suppressLightGridCompute = true; sceneDescriptor.suppressHZBOcclusion = true;
        renderer->AddDescriptor(std::move(sceneDescriptor));
        renderer->BeginFrame(frame); renderer->DrawFrame(); renderer->EndFrame();
        ASSERT_TRUE(context::DriverTestAccess::TryDrainThreadedRendering(driver));
    }
    ASSERT_FALSE(observed.empty());

    const auto& package = observed.back();
    const auto pass = std::find_if(package.passCommandInputs.begin(),package.passCommandInputs.end(),
        [](const auto& input) { return input.debugName == "VfxSceneSprites"; });
    ASSERT_NE(pass,package.passCommandInputs.end());
    ASSERT_EQ(pass->recordedDrawCommands.size(),2u);
    // The far emitter owns two particles and must be submitted first.
    const auto particleCountOf = [&](const auto& command) -> std::optional<VFX::ParticleViewStatistics>
    {
        VFX::ParticleViewStatistics statistics;
        rhi::RHIBufferReadbackDesc request; request.source = command.indirectArguments;
        request.size = sizeof(statistics); request.data = &statistics;
        const auto read = device->BeginReadBuffer(request);
        EXPECT_TRUE(read.Succeeded()) << read.message;
        if (!read.Succeeded()) return std::nullopt;
        if (read.completion) EXPECT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
        return statistics;
    };
    if (gpuEmitters)
    {
        const auto& firstPackage = observed.front();
        const auto firstPass = std::find_if(firstPackage.passCommandInputs.begin(),firstPackage.passCommandInputs.end(),
            [](const auto& input) { return input.debugName == "VfxSceneSprites"; });
        ASSERT_NE(firstPass,firstPackage.passCommandInputs.end());
        ASSERT_EQ(firstPass->recordedDrawCommands.size(),2u);
        const auto fallbackFirst = particleCountOf(firstPass->recordedDrawCommands[0]);
        const auto fallbackSecond = particleCountOf(firstPass->recordedDrawCommands[1]);
        ASSERT_TRUE(fallbackFirst.has_value()); ASSERT_TRUE(fallbackSecond.has_value());
        EXPECT_EQ(fallbackFirst->particleCount,1u)
            << "Before the first bounds readback completes, declaration order is the fallback";
        EXPECT_EQ(fallbackSecond->particleCount,2u);
    }
    const auto first = particleCountOf(pass->recordedDrawCommands[0]);
    const auto second = particleCountOf(pass->recordedDrawCommands[1]);
    ASSERT_TRUE(first.has_value()); ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->particleCount,2u) << "Transparent draws must be ordered far to near";
    EXPECT_EQ(second->particleCount,1u);

    std::vector<uint8_t> pixels(128*128*4);
    const auto read = device->BeginReadPixels(color,0,0,128,128,Render::Settings::EPixelDataFormat::RGBA,
        Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
    ASSERT_TRUE(read.Succeeded()) << read.message;
    if (read.completion) ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
    const auto center = (64u*128u+64u)*4u;
    // Far red composited first leaves the near blue with the larger share.
    EXPECT_GT(pixels[center+2],pixels[center]) << "Near particles were composited before the far ones";
    EXPECT_GT(pixels[center+2],32u);
    if (vulkan) EXPECT_EQ(Render::Backend::GetVulkanValidationErrorCountForTesting(),validationBefore);
}

TEST(VfxSceneTests, Dx12TransparentParticlesOrderEmittersByViewDepth)
{
    ValidateTransparentEmitterOrdering(false);
}

TEST(VfxSceneTests, VulkanTransparentParticlesOrderEmittersByViewDepth)
{
    ValidateTransparentEmitterOrdering(true);
}

TEST(VfxSceneTests, Dx12TransparentGpuEmittersOrderSameComponentByReadbackDepth)
{
#if defined(_WIN32)
    ValidateTransparentEmitterOrdering(false,true);
#else
    GTEST_SKIP() << "DX12 requires Windows";
#endif
}

TEST(VfxSceneTests, VulkanTransparentGpuEmittersOrderSameComponentByReadbackDepth)
{
    ValidateTransparentEmitterOrdering(true,true);
}
