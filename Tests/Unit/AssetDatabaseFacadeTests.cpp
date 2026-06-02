#include <gtest/gtest.h>

#ifndef NLS_HAS_AUTODESK_FBX_SDK
#define NLS_HAS_AUTODESK_FBX_SDK 0
#endif

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetDragDropWorkflow.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/PrefabEditorWorkflow.h"
#include "Core/ServiceLocator.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "GameObject.h"
#include "Guid.h"
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "SceneSystem/Scene.h"
#include "Serialize/PPtr.h"

#include <Json/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
std::filesystem::path MakeAssetDatabaseFacadeRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_database_facade_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets");
    return root;
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> TinyPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C,
        0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00,
        0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0x4A, 0x3B,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

std::string ReadArtifactPayloadText(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    const auto bytes = ReadBinaryFile(path);
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(bytes, artifactType, schemaVersion);
    if (!container.has_value())
        return {};

    return std::string(container->payload.begin(), container->payload.end());
}

NLS::Core::Assets::ImportedArtifact MakeArtifact(
    NLS::Core::Assets::AssetId owner,
    std::string subAssetKey,
    NLS::Core::Assets::ArtifactType type,
    std::string loaderId,
    std::string artifactPath = {},
    std::string contentHash = {},
    std::string targetPlatform = "editor")
{
    if (artifactPath.empty())
        artifactPath = "Library/Artifacts/" + owner.ToString() + "/" + subAssetKey;
    if (contentHash.empty())
        contentHash = "sha256:" + owner.ToString() + ":" + subAssetKey;

    return {
        owner,
        std::move(subAssetKey),
        type,
        std::move(loaderId),
        std::move(targetPlatform),
        std::move(artifactPath),
        std::move(contentHash)
    };
}

NLS::Core::Assets::AssetId ParseAssetId(const std::string& guid)
{
    return NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
}

template <typename T>
NLS::Engine::Serialize::PPtr<T> MakePPtr(const NLS::Engine::Serialize::ObjectIdentifier& identifier)
{
    return NLS::Engine::Serialize::PPtr<T>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
}

const NLS::Engine::Assets::RuntimeAssetPack* FindPack(
    const NLS::Engine::Assets::RuntimeAssetManifest& manifest,
    const std::string& packName,
    const std::string& packVariant)
{
    const auto found = std::find_if(
        manifest.assetPacks.begin(),
        manifest.assetPacks.end(),
        [&packName, &packVariant](const NLS::Engine::Assets::RuntimeAssetPack& pack)
        {
            return pack.packName == packName && pack.packVariant == packVariant;
        });
    return found != manifest.assetPacks.end() ? &(*found) : nullptr;
}

const NLS::Engine::Assets::RuntimeAssetPackEntry* FindPackEntry(
    const NLS::Engine::Assets::RuntimeAssetPack& pack,
    NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey)
{
    const auto found = std::find_if(
        pack.entries.begin(),
        pack.entries.end(),
        [&assetId, &subAssetKey](const NLS::Engine::Assets::RuntimeAssetPackEntry& entry)
        {
            return entry.reference.assetId == assetId && entry.reference.subAssetKey == subAssetKey;
        });
    return found != pack.entries.end() ? &(*found) : nullptr;
}

bool ContainsDependency(
    const std::vector<NLS::Engine::Assets::RuntimeAssetRef>& dependencies,
    NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey)
{
    return std::any_of(
        dependencies.begin(),
        dependencies.end(),
        [&assetId, &subAssetKey](const NLS::Engine::Assets::RuntimeAssetRef& dependency)
        {
            return dependency.assetId == assetId && dependency.subAssetKey == subAssetKey;
        });
}

bool ContainsAssetDiagnosticCode(
    const NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const std::string& code)
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [&code](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.code == code;
        });
}

bool ContainsManifestDependency(
    const nlohmann::json& manifest,
    const std::string& kind,
    const std::string& value)
{
    const auto dependencies = manifest.find("dependencies");
    if (dependencies == manifest.end() || !dependencies->is_array())
        return false;

    return std::any_of(
        dependencies->begin(),
        dependencies->end(),
        [&kind, &value](const nlohmann::json& dependency)
        {
            return dependency.is_object() &&
                dependency.value("kind", std::string {}) == kind &&
                dependency.value("value", std::string {}) == value &&
                !dependency.value("hashOrVersion", std::string {}).empty();
        });
}

class TextureReimportTestAdapter final : public NLS::Render::RHI::RHIAdapter
{
public:
    std::string_view GetDebugName() const override { return "TextureReimportTestAdapter"; }
    NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::DX12; }
    std::string_view GetVendor() const override { return "TestVendor"; }
    std::string_view GetHardware() const override { return "TestHardware"; }
};

class TextureReimportTestTexture final : public NLS::Render::RHI::RHITexture
{
public:
    explicit TextureReimportTestTexture(NLS::Render::RHI::RHITextureDesc desc)
        : m_desc(std::move(desc))
    {
    }

    std::string_view GetDebugName() const override { return m_desc.debugName; }
    const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
    NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }

private:
    NLS::Render::RHI::RHITextureDesc m_desc {};
};

class TextureReimportTestTextureView final : public NLS::Render::RHI::RHITextureView
{
public:
    TextureReimportTestTextureView(
        std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
        NLS::Render::RHI::RHITextureViewDesc desc)
        : m_texture(std::move(texture))
        , m_desc(std::move(desc))
    {
    }

    std::string_view GetDebugName() const override { return m_desc.debugName; }
    const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
    const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

private:
    std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
    NLS::Render::RHI::RHITextureViewDesc m_desc {};
};

class TextureReimportTestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
{
public:
    explicit TextureReimportTestCommandBuffer(std::string debugName)
        : m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    void Begin() override { m_recording = true; m_closed = false; }
    void End() override { m_recording = false; m_closed = true; }
    void Reset() override { m_recording = false; m_closed = false; }
    bool IsRecording() const override { return m_recording; }
    bool IsClosedForSubmission() const override { return m_closed; }
    NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
    void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc&) override {}
    void EndRenderPass() override {}
    void SetViewport(const NLS::Render::RHI::RHIViewport&) override {}
    void SetScissor(const NLS::Render::RHI::RHIRect2D&) override {}
    void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>&) override {}
    void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}
    void BindBindingSet(uint32_t, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>&) override {}
    void PushConstants(NLS::Render::RHI::ShaderStageMask, uint32_t, uint32_t, const void*) override {}
    void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override {}
    void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override {}
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
    void Dispatch(uint32_t, uint32_t, uint32_t) override {}
    void CopyBuffer(
        const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
        const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
        const NLS::Render::RHI::RHIBufferCopyRegion&) override {}
    void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc&) override {}
    void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
    void Barrier(const NLS::Render::RHI::RHIBarrierDesc&) override {}

private:
    std::string m_debugName;
    bool m_recording = false;
    bool m_closed = false;
};

class TextureReimportTestCommandPool final : public NLS::Render::RHI::RHICommandPool
{
public:
    TextureReimportTestCommandPool(NLS::Render::RHI::QueueType queueType, std::string debugName)
        : m_queueType(queueType)
        , m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
    std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName = {}) override
    {
        return std::make_shared<TextureReimportTestCommandBuffer>(std::move(debugName));
    }
    void Reset() override {}

private:
    NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;
    std::string m_debugName;
};

class TextureReimportTestFence final : public NLS::Render::RHI::RHIFence
{
public:
    explicit TextureReimportTestFence(std::string debugName)
        : m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    bool IsSignaled() const override { return m_signaled; }
    void Reset() override { m_signaled = false; }
    bool Wait(uint64_t = 0u) override
    {
        m_signaled = true;
        return true;
    }

private:
    std::string m_debugName;
    bool m_signaled = true;
};

class TextureReimportTestSemaphore final : public NLS::Render::RHI::RHISemaphore
{
public:
    explicit TextureReimportTestSemaphore(std::string debugName)
        : m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    bool IsSignaled() const override { return false; }
    void Reset() override {}

private:
    std::string m_debugName;
};

class TextureReimportTestDevice final : public NLS::Render::RHI::RHIDevice
{
public:
    TextureReimportTestDevice()
        : m_adapter(std::make_shared<TextureReimportTestAdapter>())
    {
        m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
        m_capabilities.backendReady = true;
        m_capabilities.supportsGraphics = true;
        m_capabilities.supportsCurrentSceneRenderer = true;
        for (const auto& descriptor : NLS::Render::RHI::kTextureFormatDescriptors)
        {
            m_capabilities.SetTextureFormatCapability(
                descriptor.format,
                {
                    descriptor.format,
                    descriptor.sampled,
                    descriptor.supportsUpload,
                    descriptor.colorAttachment,
                    descriptor.storage,
                    descriptor.supportsSrgbView,
                    descriptor.requiresAlignedTopLevelBlocks,
                    true,
                    {}
                });
        }
    }

    std::string_view GetDebugName() const override { return "TextureReimportTestDevice"; }
    const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
    const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
    NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
    bool IsBackendReady() const override { return true; }
    std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
        const NLS::Render::RHI::RHIBufferDesc&,
        const NLS::Render::RHI::RHIBufferUploadDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
        const NLS::Render::RHI::RHITextureDesc& desc,
        const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc) override
    {
        ++textureCreateCalls;
        lastTextureDesc = desc;
        lastTextureUploadDesc = uploadDesc;
        return std::make_shared<TextureReimportTestTexture>(desc);
    }
    std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
        const NLS::Render::RHI::RHITextureViewDesc& desc) override
    {
        lastTextureViewDesc = desc;
        return std::make_shared<TextureReimportTestTextureView>(texture, desc);
    }
    std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc&, std::string = {}) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
        NLS::Render::RHI::QueueType queueType,
        std::string debugName = {}) override
    {
        return std::make_shared<TextureReimportTestCommandPool>(queueType, std::move(debugName));
    }
    std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName = {}) override
    {
        return std::make_shared<TextureReimportTestFence>(std::move(debugName));
    }
    std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName = {}) override
    {
        return std::make_shared<TextureReimportTestSemaphore>(std::move(debugName));
    }
    void ReadPixels(
        const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
        uint32_t,
        uint32_t,
        uint32_t,
        uint32_t,
        NLS::Render::Settings::EPixelDataFormat,
        NLS::Render::Settings::EPixelDataType,
        void*) override {}

    size_t textureCreateCalls = 0u;
    NLS::Render::RHI::RHITextureDesc lastTextureDesc {};
    NLS::Render::RHI::RHITextureUploadDesc lastTextureUploadDesc {};
    NLS::Render::RHI::RHITextureViewDesc lastTextureViewDesc {};

private:
    std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
    NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
    NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
};

class ScopedAssetDatabaseFacadeDriverService final
{
public:
    explicit ScopedAssetDatabaseFacadeDriverService(NLS::Render::Context::Driver& driver)
    {
        NLS::Core::ServiceLocator::Provide(driver);
    }

    ~ScopedAssetDatabaseFacadeDriverService()
    {
        NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
    }
};

NLS::Render::Context::Driver& EnsureAssetDatabaseFacadeTestDriver()
{
    static auto driver = std::make_unique<NLS::Render::Context::Driver>([]()
    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        return settings;
    }());
    NLS::Core::ServiceLocator::Provide(*driver);
    return *driver;
}
}

TEST(AssetDatabaseFacadeTests, GuidPathAndMainSubAssetQueriesMatchEditorWorkflow)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto guid = database.AssetPathToGUID("Assets/Models/Hero.gltf");
    ASSERT_FALSE(guid.empty());
    EXPECT_EQ(database.GUIDToAssetPath(guid), "Assets/Models/Hero.gltf");
    EXPECT_TRUE(database.AssetPathToGUID("Assets/Missing.gltf").empty());

    const auto modelId = ParseAssetId(guid);
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "model:Hero";
    manifest.subAssets.push_back(MakeArtifact(modelId, "model:Hero", ArtifactType::Model, "model"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "material:Body", ArtifactType::Material, "material"));
    database.AddArtifactManifest(manifest);

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->assetId, modelId);
    EXPECT_EQ(mainAsset->subAssetKey, "model:Hero");
    EXPECT_EQ(mainAsset->artifactType, ArtifactType::Model);
    EXPECT_TRUE(mainAsset->mainAsset);

    const auto allAssets = database.LoadAllAssetsAtPath("Assets/Models/Hero.gltf");
    ASSERT_EQ(allAssets.size(), 3u);
    EXPECT_EQ(allAssets[0].subAssetKey, "model:Hero");
    EXPECT_EQ(allAssets[1].subAssetKey, "mesh:Body");
    EXPECT_EQ(allAssets[2].subAssetKey, "material:Body");

    const auto mesh = database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "mesh:Body");
    ASSERT_TRUE(mesh.has_value());
    EXPECT_EQ(mesh->artifactType, ArtifactType::Mesh);
    EXPECT_FALSE(database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "mesh:Missing").has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RefreshScansAllConfiguredAssetRoots)
{
    using namespace NLS::Editor::Assets;

    const auto projectRoot = MakeAssetDatabaseFacadeRoot();
    const auto engineRoot =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_database_engine_root_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(engineRoot / "EngineAssets");
    WriteTextFile(projectRoot / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(engineRoot / "EngineAssets" / "Materials" / "Default.mat", "material");

    AssetDatabaseFacade database({projectRoot, engineRoot});
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.AssetPathToGUID("Assets/Models/Hero.gltf").empty());
    const auto engineMaterialGuid = database.AssetPathToGUID("EngineAssets/Materials/Default.mat");
    ASSERT_FALSE(engineMaterialGuid.empty());
    EXPECT_EQ(database.GUIDToAssetPath(engineMaterialGuid), "EngineAssets/Materials/Default.mat");

    std::filesystem::remove_all(projectRoot);
    std::filesystem::remove_all(engineRoot);
}

TEST(AssetDatabaseFacadeTests, FileOperationsPreserveOrRegenerateMetaIdentity)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Materials" / "Hero.mat", "material");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto originalGuid = database.AssetPathToGUID("Assets/Materials/Hero.mat");
    ASSERT_FALSE(originalGuid.empty());

    ASSERT_TRUE(database.MoveAsset("Assets/Materials/Hero.mat", "Assets/Materials/RenamedHero.mat"));
    EXPECT_EQ(database.AssetPathToGUID("Assets/Materials/RenamedHero.mat"), originalGuid);

    ASSERT_TRUE(database.RenameAsset("Assets/Materials/RenamedHero.mat", "FinalHero.mat"));
    EXPECT_EQ(database.AssetPathToGUID("Assets/Materials/FinalHero.mat"), originalGuid);

    ASSERT_TRUE(database.CopyAsset("Assets/Materials/FinalHero.mat", "Assets/Materials/CopyHero.mat"));
    const auto copyGuid = database.AssetPathToGUID("Assets/Materials/CopyHero.mat");
    ASSERT_FALSE(copyGuid.empty());
    EXPECT_NE(copyGuid, originalGuid);

    ASSERT_TRUE(database.DeleteAsset("Assets/Materials/CopyHero.mat"));
    EXPECT_TRUE(database.AssetPathToGUID("Assets/Materials/CopyHero.mat").empty());
    EXPECT_TRUE(database.GUIDToAssetPath(copyGuid).empty());

    EXPECT_EQ(database.CreateFolder("Assets", "Prefabs"), "Assets/Prefabs");
    EXPECT_TRUE(database.IsValidFolder("Assets/Prefabs"));
    WriteTextFile(root / "Assets" / "Prefabs" / "Lamp.prefab", "{}");
    EXPECT_EQ(database.GenerateUniqueAssetPath("Assets/Prefabs/Lamp.prefab"), "Assets/Prefabs/Lamp 1.prefab");

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FileOperationsRejectPathsOutsideAssetRoots)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto outside = root.parent_path() / ("outside_" + NLS::Guid::New().ToString() + ".mat");
    const auto movedName = "MovedHero_" + NLS::Guid::New().ToString() + ".mat";
    const auto escapedFolder = "Escaped_" + NLS::Guid::New().ToString();
    const auto escapedRename = "EscapedHero_" + NLS::Guid::New().ToString() + ".mat";
    const auto nestedRename = "NestedHero_" + NLS::Guid::New().ToString() + ".mat";
    const auto nestedFolder = "NestedFolder_" + NLS::Guid::New().ToString();
    WriteTextFile(root / "Assets" / "Materials" / "Hero.mat", "material");
    WriteTextFile(outside, "outside");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.CopyAsset("../" + outside.filename().generic_string(), "Assets/Materials/Stolen.mat"));
    EXPECT_FALSE(database.MoveAsset("Assets/Materials/Hero.mat", "../" + movedName));
    EXPECT_FALSE(database.DeleteAsset("../" + outside.filename().generic_string()));
    EXPECT_FALSE(database.DeleteAsset(""));
    EXPECT_FALSE(database.DeleteAsset("."));
    EXPECT_EQ(database.CreateFolder("..", escapedFolder), "");
    EXPECT_FALSE(database.RenameAsset("Assets/Materials/Hero.mat", "../" + escapedRename));
    EXPECT_FALSE(database.RenameAsset("Assets/Materials/Hero.mat", "Nested/" + nestedRename));
    EXPECT_EQ(database.CreateFolder("Assets", "../" + escapedFolder), "");
    EXPECT_EQ(database.CreateFolder("Assets", "Nested/" + nestedFolder), "");
    EXPECT_FALSE(std::filesystem::exists(root.parent_path() / movedName));
    EXPECT_FALSE(std::filesystem::exists(root.parent_path() / escapedFolder));
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / escapedRename));
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Materials" / "Nested" / nestedRename));
    EXPECT_FALSE(std::filesystem::exists(root / escapedFolder));
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Nested" / nestedFolder));
    EXPECT_TRUE(std::filesystem::exists(outside));
    EXPECT_TRUE(std::filesystem::exists(root));
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Materials/Hero.mat").empty());

    std::filesystem::remove_all(outside);
    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FileOperationsRejectReadOnlyRootsAndPathAliases)
{
    using namespace NLS::Editor::Assets;

    const auto projectRoot = MakeAssetDatabaseFacadeRoot();
    const auto packageRoot =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_database_readonly_root_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(packageRoot / "Assets" / "Shared");
    std::filesystem::create_directories(packageRoot / "Packages" / "Starter");
    WriteTextFile(projectRoot / "Assets" / "Shared" / "Hero.mat", "project");
    WriteTextFile(packageRoot / "Assets" / "Shared" / "Hero.mat", "package");
    WriteTextFile(packageRoot / "Packages" / "Starter" / "ReadOnly.mat", "readonly");

    AssetDatabaseFacade database({
        {projectRoot, false},
        {packageRoot, true}
    });
    EXPECT_FALSE(database.Refresh());
    EXPECT_TRUE(ContainsAssetDiagnosticCode(database.GetDiagnostics(), "assetdatabase-editor-path-alias"));

    EXPECT_FALSE(database.DeleteAsset("Packages/Starter/ReadOnly.mat"));
    EXPECT_FALSE(database.CreateTextAsset("new", "Packages/Starter/NewReadonly.mat"));
    EXPECT_TRUE(std::filesystem::exists(packageRoot / "Packages" / "Starter" / "ReadOnly.mat"));
    EXPECT_FALSE(std::filesystem::exists(packageRoot / "Packages" / "Starter" / "NewReadonly.mat"));

    std::filesystem::remove_all(projectRoot);
    std::filesystem::remove_all(packageRoot);
}

TEST(AssetDatabaseFacadeTests, MetadataOperationsRejectNestedReadOnlyRoots)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    std::filesystem::create_directories(root / "Packages" / "Starter");
    WriteTextFile(root / "Packages" / "Starter" / "ReadOnly.mat", "readonly");

    AssetDatabaseFacade database({
        {root, false, {}},
        {root / "Packages", true, "Packages"}
    });
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.DeleteAsset("Packages/Starter/ReadOnly.mat"));
    EXPECT_FALSE(database.SetLabels("Packages/Starter/ReadOnly.mat", {"locked"}));
    EXPECT_FALSE(database.SetAssetPackNameAndVariant("Packages/Starter/ReadOnly.mat", "locked", ""));
    EXPECT_FALSE(database.CreateTextAsset("new", "Packages/Starter/New.mat"));
    EXPECT_TRUE(std::filesystem::exists(root / "Packages" / "Starter" / "ReadOnly.mat"));
    EXPECT_FALSE(std::filesystem::exists(root / "Packages" / "Starter" / "New.mat"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, EmptyOrFilesystemRootConfiguredRootsAreRejected)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Materials" / "Hero.mat", "material");

    AssetDatabaseFacade database({
        {{}, false, {}},
        {root.root_path(), false, {}},
        {root, false, {}}
    });
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.DeleteAsset(""));
    EXPECT_FALSE(database.DeleteAsset("."));
    EXPECT_TRUE(std::filesystem::exists(root));
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Materials/Hero.mat").empty());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FileOperationsRejectSymlinkEscapesWhenSupported)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto outside =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_database_symlink_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets");
    std::filesystem::create_directories(outside);
    WriteTextFile(outside / "Outside.mat", "outside");

    std::error_code error;
    std::filesystem::create_directory_symlink(outside, root / "Assets" / "LinkedOutside", error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "Directory symlink creation is not available in this environment.";
    }

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    EXPECT_FALSE(database.DeleteAsset("Assets/LinkedOutside/Outside.mat"));
    EXPECT_FALSE(database.CreateTextAsset("new", "Assets/LinkedOutside/New.mat"));
    EXPECT_TRUE(std::filesystem::exists(outside / "Outside.mat"));
    EXPECT_FALSE(std::filesystem::exists(outside / "New.mat"));

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetDatabaseFacadeTests, FileOperationsCreateNewAssetsInMatchingNonPrimaryRoot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto projectRoot = MakeAssetDatabaseFacadeRoot();
    const auto engineRoot =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_database_engine_write_root_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(engineRoot / "EngineAssets");

    AssetDatabaseFacade database({projectRoot, engineRoot});
    ASSERT_TRUE(database.Refresh());

    const auto assetId = ParseAssetId("e2020202-0202-4202-8202-020202020202");
    ASSERT_TRUE(database.CreateTextAsset("generated", "EngineAssets/Generated/Tool.asset", assetId));

    EXPECT_TRUE(std::filesystem::exists(engineRoot / "EngineAssets" / "Generated" / "Tool.asset"));
    EXPECT_FALSE(std::filesystem::exists(projectRoot / "EngineAssets" / "Generated" / "Tool.asset"));
    EXPECT_EQ(database.AssetPathToGUID("EngineAssets/Generated/Tool.asset"), assetId.ToString());

    WriteTextFile(projectRoot / "Assets" / "Materials" / "Hero.mat", "material");
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.CopyAsset("Assets/Materials/Hero.mat", "EngineAssets/Materials/HeroCopy.mat"));

    EXPECT_TRUE(std::filesystem::exists(engineRoot / "EngineAssets" / "Materials" / "HeroCopy.mat"));
    EXPECT_FALSE(std::filesystem::exists(projectRoot / "EngineAssets" / "Materials" / "HeroCopy.mat"));

    std::filesystem::remove_all(projectRoot);
    std::filesystem::remove_all(engineRoot);
}

TEST(AssetDatabaseFacadeTests, RefreshAndImportBatchingQueueWorkUntilStopAssetEditing)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Textures" / "Existing.png", "png");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_FALSE(database.AssetPathToGUID("Assets/Textures/Existing.png").empty());

    database.StartAssetEditing();
    WriteTextFile(
        root / "Assets" / "Models" / "Queued.obj",
        R"(
o Queued
v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
)");
    EXPECT_TRUE(database.ImportAsset("Assets/Models/Queued.obj"));
    EXPECT_EQ(database.GetQueuedImportCount(), 1u);
    EXPECT_EQ(database.GetCompletedImportCount(), 0u);
    EXPECT_TRUE(database.AssetPathToGUID("Assets/Models/Queued.obj").empty());

    EXPECT_TRUE(database.StopAssetEditing());
    EXPECT_EQ(database.GetQueuedImportCount(), 0u);
    EXPECT_EQ(database.GetCompletedImportCount(), 1u);
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Models/Queued.obj").empty());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportModelSceneWritesInternalArtifactsAndGeneratedPrefabSubAsset)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorFactor": [0.8, 0.7, 0.6, 1.0],
                        "metallicFactor": 0.25,
                        "roughnessFactor": 0.5
                    }
                }
            ],
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    const auto allAssets = database.LoadAllAssetsAtPath("Assets/Models/Hero.gltf");
    ASSERT_EQ(allAssets.size(), 3u);

    const auto hasSubAsset = [&allAssets](const std::string& key, ArtifactType type)
    {
        return std::any_of(
            allAssets.begin(),
            allAssets.end(),
            [&key, type](const AssetDatabaseRecord& record)
            {
                return record.subAssetKey == key && record.artifactType == type;
            });
    };

    EXPECT_TRUE(hasSubAsset("material:material/0", ArtifactType::Material));
    EXPECT_TRUE(hasSubAsset("mesh:mesh/0", ArtifactType::Mesh));
    EXPECT_TRUE(hasSubAsset("prefab:Hero", ArtifactType::Prefab));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->subAssetKey, "prefab:Hero");
    EXPECT_EQ(mainAsset->artifactType, ArtifactType::Prefab);

    const auto artifactRoot = root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Models/Hero.gltf");
    EXPECT_FALSE(std::filesystem::exists(artifactRoot / "model.nmodel"));
    EXPECT_TRUE(std::filesystem::exists(artifactRoot / "prefab.nprefab"));
    const auto meshArtifactPath = artifactRoot / "meshes" / "mesh%3Amesh%2F0.nmesh";
    EXPECT_TRUE(std::filesystem::exists(meshArtifactPath));
    EXPECT_TRUE(std::filesystem::exists(artifactRoot / "materials" / "material%3Amaterial%2F0.nmat"));

    const auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshArtifactPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);
    EXPECT_EQ(meshArtifact->indices.size(), 3u);
    EXPECT_EQ(meshArtifact->materialIndex, 0u);
    EXPECT_FLOAT_EQ(meshArtifact->vertices[1].position[0], 1.0f);
    EXPECT_FLOAT_EQ(meshArtifact->vertices[2].position[1], 1.0f);

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB" / "index.tsv"));
    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    const auto* meshRecord = artifactDatabase.Find(sourceId, "mesh:mesh/0", "editor");
    ASSERT_NE(meshRecord, nullptr);
    EXPECT_EQ(meshRecord->sourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(meshRecord->artifactPath, "Library/Artifacts/" + sourceId.ToString() + "/meshes/mesh%3Amesh%2F0.nmesh");
    EXPECT_EQ(meshRecord->loaderId, "mesh");
    EXPECT_EQ(meshRecord->status, NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    EXPECT_EQ(artifactDatabase.FindBySource(sourceId).size(), allAssets.size());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ProjectLibraryArtifactDatabaseStoresModelMaterialAndTexturePathsRelativeToProject)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "materials": [
                { "name": "Body" }
            ],
            "meshes": [
                {
                    "name": "HeroMesh",
                    "primitives": [
                        { "attributes": {}, "material": 0 }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB" / "index.tsv"));
    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    const auto records = artifactDatabase.FindBySource(sourceId);
    ASSERT_FALSE(records.empty());

    bool sawMaterial = false;
    bool sawMesh = false;
    bool sawPrefab = false;
    for (const auto* record : records)
    {
        ASSERT_NE(record, nullptr);
        EXPECT_FALSE(std::filesystem::path(record->artifactPath).is_absolute()) << record->artifactPath;
        EXPECT_EQ(record->artifactPath.find("Library/Artifacts/" + sourceId.ToString() + "/"), 0u)
            << record->artifactPath;
        EXPECT_EQ(record->artifactPath.find('\\'), std::string::npos) << record->artifactPath;

        sawMaterial = sawMaterial || record->artifactType == ArtifactType::Material;
        sawMesh = sawMesh || record->artifactType == ArtifactType::Mesh;
        sawPrefab = sawPrefab || record->artifactType == ArtifactType::Prefab;
    }

    EXPECT_TRUE(sawMaterial);
    EXPECT_TRUE(sawMesh);
    EXPECT_TRUE(sawPrefab);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedModelMaterialReferencesPreimportedShaderArtifactHandle)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Engine" / "Shaders" / "StandardPBR.hlsl",
        R"(
float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
)");
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "materials": [{ "name": "Body" }],
            "meshes": [{ "name": "HeroMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "HeroRoot", "mesh": 0 }]
        })");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Engine/Shaders/StandardPBR.hlsl"));
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    const auto modelManifest = database.GetArtifactManifestForAssetPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(modelManifest.has_value());
    const auto* materialArtifact = modelManifest->FindSubAsset("material:material/0");
    ASSERT_NE(materialArtifact, nullptr);

    const auto materialPayload = ReadArtifactPayloadText(
        materialArtifact->artifactPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    const auto shaderId = database.AssetPathToGUID("Assets/Engine/Shaders/StandardPBR.hlsl");
    ASSERT_FALSE(shaderId.empty());
    EXPECT_NE(
        materialPayload.find("<shader>Library/Artifacts/" + shaderId + "/shader.nshader</shader>"),
        std::string::npos);
    EXPECT_EQ(materialPayload.find(":Shaders/StandardPBR.hlsl"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportShaderSourceWritesShaderArtifactManifestAndCentralIndex)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "Shader artifact import success currently requires Windows DXC process execution.";
#else
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "HeroSurface.hlsl",
        R"(
float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
)");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/HeroSurface.hlsl"));

    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Shaders/HeroSurface.hlsl"));
    ASSERT_TRUE(sourceId.IsValid());

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/HeroSurface.hlsl");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->subAssetKey, "shader:HeroSurface");
    EXPECT_EQ(mainAsset->artifactType, ArtifactType::Shader);
    EXPECT_TRUE(mainAsset->mainAsset);
    EXPECT_EQ(
        mainAsset->artifactPath,
        (root / "Library" / "Artifacts" / sourceId.ToString() / "shader.nshader").string());

    const auto artifactPayload = ReadTextFile(mainAsset->artifactPath);
    ASSERT_FALSE(artifactPayload.empty());
    EXPECT_NE(artifactPayload.find("NULLUS_IMPORTED_SHADER_ARTIFACT=1"), std::string::npos);
    EXPECT_NE(artifactPayload.find("SOURCE=Assets/Shaders/HeroSurface.hlsl"), std::string::npos);
    EXPECT_NE(artifactPayload.find("SUB_ASSET=shader:HeroSurface"), std::string::npos);
    EXPECT_NE(artifactPayload.find("ENTRY=VSMain"), std::string::npos);
    EXPECT_NE(artifactPayload.find("ENTRY=PSMain"), std::string::npos);
    EXPECT_NE(artifactPayload.find("TARGET=GLSL"), std::string::npos);
    EXPECT_NE(artifactPayload.find("PROFILE=glsl_430"), std::string::npos);

    const auto shaderArtifact = NLS::Render::Assets::LoadShaderArtifact(mainAsset->artifactPath);
    ASSERT_TRUE(shaderArtifact.has_value());
    EXPECT_TRUE(std::any_of(
        shaderArtifact->stages.begin(),
        shaderArtifact->stages.end(),
        [](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.stage == NLS::Render::ShaderCompiler::ShaderStage::Vertex &&
                stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL &&
                stage.output.status == NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
                !stage.output.bytecode.empty();
        }));
    EXPECT_TRUE(std::any_of(
        shaderArtifact->stages.begin(),
        shaderArtifact->stages.end(),
        [](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.stage == NLS::Render::ShaderCompiler::ShaderStage::Pixel &&
                stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL &&
                stage.output.status == NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
                !stage.output.bytecode.empty();
        }));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/HeroSurface.hlsl");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ(manifest->sourceAssetId, sourceId);
    EXPECT_EQ(manifest->importerId, "shader");
    EXPECT_EQ(manifest->primarySubAssetKey, "shader:HeroSurface");
    ASSERT_NE(manifest->FindSubAsset("shader:HeroSurface"), nullptr);
    EXPECT_TRUE(std::any_of(
        manifest->dependencies.begin(),
        manifest->dependencies.end(),
        [](const AssetDependencyRecord& dependency)
        {
            return dependency.kind == AssetDependencyKind::BuildTarget &&
                dependency.value == "editor";
        }));

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB" / "index.tsv"));
    const auto* record = artifactDatabase.Find(sourceId, "shader:HeroSurface", "editor");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->sourcePath, "Assets/Shaders/HeroSurface.hlsl");
    EXPECT_EQ(record->artifactType, ArtifactType::Shader);
    EXPECT_EQ(record->loaderId, "shader");
    EXPECT_EQ(record->artifactPath, "Library/Artifacts/" + sourceId.ToString() + "/shader.nshader");

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetDatabaseFacadeTests, ShaderArtifactRoundTripsDependencyPathsWithSemicolons)
{
    NLS::Render::Assets::ShaderArtifact artifact;
    artifact.sourcePath = "Assets/Shaders/HeroSurface.hlsl";
    artifact.subAssetKey = "shader:HeroSurface";

    NLS::Render::Assets::ShaderArtifactStage stage;
    stage.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
    stage.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    stage.entryPoint = "PSMain";
    stage.targetProfile = "ps_6_0";
    stage.output.status = NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
    stage.output.bytecode = {1u, 2u, 3u, 4u};
    stage.output.dependencyPaths = {
        "C:/Project/Assets/Shaders/Shared;Lighting.hlsli",
        "C:/Project/Assets/Shaders/Common.hlsli"
    };
    artifact.stages.push_back(std::move(stage));

    const auto serialized = NLS::Render::Assets::SerializeShaderArtifact(artifact);
    const auto restored = NLS::Render::Assets::DeserializeShaderArtifact(serialized);
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->stages.size(), 1u);
    EXPECT_EQ(restored->stages.front().output.dependencyPaths, artifact.stages.front().output.dependencyPaths);
}

TEST(AssetDatabaseFacadeTests, StartupPreimportPlanIncludesShaderSourceAssetsAndSkipsWarmShaderArtifacts)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "Warm shader artifact preimport currently requires Windows DXC process execution.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "Warmup.hlsl",
        R"(
float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
)");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    auto coldPlan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_NE(
        std::find(coldPlan.assetPaths.begin(), coldPlan.assetPaths.end(), "Assets/Shaders/Warmup.hlsl"),
        coldPlan.assetPaths.end());

    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    auto warmPlan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_EQ(
        std::find(warmPlan.assetPaths.begin(), warmPlan.assetPaths.end(), "Assets/Shaders/Warmup.hlsl"),
        warmPlan.assetPaths.end());

    WriteTextFile(
        root / "Assets" / "Shaders" / "Warmup.hlsl",
        R"(
float4 VSMain() : SV_Position { return float4(1, 0, 0, 1); }
float4 PSMain() : SV_Target { return float4(0, 1, 0, 1); }
)");
    ASSERT_TRUE(database.Refresh());
    auto changedPlan = scheduler.BuildPlan(database, {AssetPreimportReason::FileWatcherChanged, {root / "Assets" / "Shaders" / "Warmup.hlsl"}});
    EXPECT_NE(
        std::find(changedPlan.assetPaths.begin(), changedPlan.assetPaths.end(), "Assets/Shaders/Warmup.hlsl"),
        changedPlan.assetPaths.end());

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetDatabaseFacadeTests, StartupPreimportPlanReimportsShaderArtifactsWithoutUsableStages)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Shaders" / "Broken.hlsl", "// missing entry points\n");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/Broken.hlsl"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/Broken.hlsl");
    ASSERT_TRUE(mainAsset.has_value());
    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(mainAsset->artifactPath);
    ASSERT_TRUE(artifact.has_value());
    ASSERT_FALSE(NLS::Render::Assets::HasUsableShaderArtifactStage(*artifact));

    AssetPreimportScheduler scheduler;
    auto plan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Shaders/Broken.hlsl"),
        plan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, StartupPreimportPlanReimportsShaderArtifactsMissingGlslStages)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "LegacyWarm.hlsl",
        R"(
float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
)");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/LegacyWarm.hlsl"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/LegacyWarm.hlsl");
    ASSERT_TRUE(mainAsset.has_value());
    auto shaderArtifact = NLS::Render::Assets::LoadShaderArtifact(mainAsset->artifactPath);
    ASSERT_TRUE(shaderArtifact.has_value());
    shaderArtifact->stages.erase(
        std::remove_if(
            shaderArtifact->stages.begin(),
            shaderArtifact->stages.end(),
            [](const NLS::Render::Assets::ShaderArtifactStage& stage)
            {
                return stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL;
            }),
        shaderArtifact->stages.end());
    const auto serializedShaderArtifact = NLS::Render::Assets::SerializeShaderArtifact(*shaderArtifact);
    WriteTextFile(
        mainAsset->artifactPath,
        std::string(serializedShaderArtifact.begin(), serializedShaderArtifact.end()));

    AssetPreimportScheduler scheduler;
    auto plan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Shaders/LegacyWarm.hlsl"),
        plan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactDatabaseKeepsConcurrentManifestRecords)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "HeroA.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(root / "Assets" / "Models" / "HeroB.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto heroAId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroA.gltf"));
    const auto heroBId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroB.gltf"));
    ASSERT_TRUE(heroAId.IsValid());
    ASSERT_TRUE(heroBId.IsValid());

    ArtifactManifest heroA;
    heroA.sourceAssetId = heroAId;
    heroA.importerId = "scene-model";
    heroA.targetPlatform = "editor";
    heroA.primarySubAssetKey = "model:HeroA";
    heroA.subAssets.push_back(MakeArtifact(heroAId, "model:HeroA", ArtifactType::Model, "model"));

    ArtifactManifest heroB;
    heroB.sourceAssetId = heroBId;
    heroB.importerId = "scene-model";
    heroB.targetPlatform = "editor";
    heroB.primarySubAssetKey = "model:HeroB";
    heroB.subAssets.push_back(MakeArtifact(heroBId, "model:HeroB", ArtifactType::Model, "model"));

    auto first = std::async(std::launch::async, [&database, heroA]()
    {
        database.AddArtifactManifest(heroA);
    });
    auto second = std::async(std::launch::async, [&database, heroB]()
    {
        database.AddArtifactManifest(heroB);
    });
    first.get();
    second.get();

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB" / "index.tsv"));
    EXPECT_NE(artifactDatabase.Find(heroAId, "model:HeroA", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(heroBId, "model:HeroB", "editor"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactDatabaseBatchUpsertsDoNotReloadCentralIndexPerManifest)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "HeroA.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(root / "Assets" / "Models" / "HeroB.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto heroAId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroA.gltf"));
    const auto heroBId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroB.gltf"));

    ArtifactManifest heroA;
    heroA.sourceAssetId = heroAId;
    heroA.importerId = "scene-model";
    heroA.targetPlatform = "editor";
    heroA.primarySubAssetKey = "model:HeroA";
    heroA.subAssets.push_back(MakeArtifact(heroAId, "model:HeroA", ArtifactType::Model, "model"));

    ArtifactManifest heroB;
    heroB.sourceAssetId = heroBId;
    heroB.importerId = "scene-model";
    heroB.targetPlatform = "editor";
    heroB.primarySubAssetKey = "model:HeroB";
    heroB.subAssets.push_back(MakeArtifact(heroBId, "model:HeroB", ArtifactType::Model, "model"));

    database.AddArtifactManifest(heroA);
    WriteTextFile(root / "Library" / "ArtifactDB" / "index.tsv", "corrupted central index\n");
    database.AddArtifactManifest(heroB);

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB" / "index.tsv"));
    EXPECT_NE(artifactDatabase.Find(heroAId, "model:HeroA", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(heroBId, "model:HeroB", "editor"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactDatabaseBatchUpsertsFlushCentralIndexOnceOnStopAssetEditing)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "HeroA.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(root / "Assets" / "Models" / "HeroB.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto heroAId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroA.gltf"));
    const auto heroBId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroB.gltf"));

    ArtifactManifest heroA;
    heroA.sourceAssetId = heroAId;
    heroA.importerId = "scene-model";
    heroA.targetPlatform = "editor";
    heroA.primarySubAssetKey = "model:HeroA";
    heroA.subAssets.push_back(MakeArtifact(heroAId, "model:HeroA", ArtifactType::Model, "model"));

    ArtifactManifest heroB;
    heroB.sourceAssetId = heroBId;
    heroB.importerId = "scene-model";
    heroB.targetPlatform = "editor";
    heroB.primarySubAssetKey = "model:HeroB";
    heroB.subAssets.push_back(MakeArtifact(heroBId, "model:HeroB", ArtifactType::Model, "model"));

    database.StartAssetEditing();
    database.AddArtifactManifest(heroA);
    database.AddArtifactManifest(heroB);
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "ArtifactDB" / "index.tsv"));
    EXPECT_TRUE(database.StopAssetEditing());

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB" / "index.tsv"));
    EXPECT_NE(artifactDatabase.Find(heroAId, "model:HeroA", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(heroBId, "model:HeroB", "editor"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactDatabaseRefreshFlushesDeferredCentralIndexBeforeClearingCache)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "HeroA.gltf", R"({"asset":{"version":"2.0"}})");
    WriteTextFile(root / "Assets" / "Models" / "HeroB.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto heroAId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroA.gltf"));
    const auto heroBId = ParseAssetId(database.AssetPathToGUID("Assets/Models/HeroB.gltf"));

    ArtifactManifest heroA;
    heroA.sourceAssetId = heroAId;
    heroA.importerId = "scene-model";
    heroA.targetPlatform = "editor";
    heroA.primarySubAssetKey = "model:HeroA";
    heroA.subAssets.push_back(MakeArtifact(heroAId, "model:HeroA", ArtifactType::Model, "model"));

    ArtifactManifest heroB;
    heroB.sourceAssetId = heroBId;
    heroB.importerId = "scene-model";
    heroB.targetPlatform = "editor";
    heroB.primarySubAssetKey = "model:HeroB";
    heroB.subAssets.push_back(MakeArtifact(heroBId, "model:HeroB", ArtifactType::Model, "model"));

    database.StartAssetEditing();
    database.AddArtifactManifest(heroA);
    database.AddArtifactManifest(heroB);
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "ArtifactDB" / "index.tsv"));

    ASSERT_TRUE(database.Refresh());

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB" / "index.tsv"));
    EXPECT_NE(artifactDatabase.Find(heroAId, "model:HeroA", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(heroBId, "model:HeroB", "editor"), nullptr);

    EXPECT_TRUE(database.StopAssetEditing());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedModelManifestReloadsInFreshFacadeAfterRefresh)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        { "attributes": { "POSITION": 0 } }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    {
        AssetDatabaseFacade importer({root});
        ASSERT_TRUE(importer.Refresh());
        ASSERT_TRUE(importer.ImportAsset("Assets/Models/Hero.gltf"));
    }

    AssetDatabaseFacade reloaded({root});
    ASSERT_TRUE(reloaded.Refresh());

    const auto allAssets = reloaded.LoadAllAssetsAtPath("Assets/Models/Hero.gltf");
    const auto hasGeneratedPrefab = std::any_of(
        allAssets.begin(),
        allAssets.end(),
        [](const AssetDatabaseRecord& record)
        {
            return record.subAssetKey == "prefab:Hero" &&
                record.artifactType == ArtifactType::Prefab &&
                !record.artifactPath.empty();
        });
    EXPECT_TRUE(hasGeneratedPrefab);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsStaleImporterMetadata)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "StaleManifestHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "StaleManifestHeroRoot" }]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/StaleManifestHero.gltf"));
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/StaleManifestHero.gltf"));

    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Models/StaleManifestHero.gltf");
    ASSERT_TRUE(manifest.has_value());
    manifest->importerVersion += 1u;
    database.AddArtifactManifest(*manifest);

    EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/StaleManifestHero.gltf"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsPreDx12TextureBuildModelImporterVersion)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "LegacyTextureBuildHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "LegacyTextureBuildHeroRoot" }]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/LegacyTextureBuildHero.gltf"));
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/LegacyTextureBuildHero.gltf"));

    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Models/LegacyTextureBuildHero.gltf");
    ASSERT_TRUE(manifest.has_value());
    manifest->importerVersion = 5u;
    database.AddArtifactManifest(*manifest);

    EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/LegacyTextureBuildHero.gltf"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsReadOnlyMetaBelowCurrentImporterVersion)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto packageRoot = root / "Packages";
    WriteTextFile(
        packageRoot / "Models" / "LegacyReadOnlyHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "LegacyReadOnlyHeroRoot" }]
        })");

    auto meta = AssetMeta::CreateForAsset(packageRoot / "Models" / "LegacyReadOnlyHero.gltf");
    meta.importerVersion = 5u;
    ASSERT_TRUE(meta.Save(packageRoot / "Models" / "LegacyReadOnlyHero.gltf.meta"));

    AssetDatabaseFacade importer({
        {packageRoot, false, "Packages", root / "Library"}
    });
    ASSERT_TRUE(importer.Refresh());
    ASSERT_TRUE(importer.ImportAsset("Packages/Models/LegacyReadOnlyHero.gltf"));
    const auto manifestPath =
        root / "Library" / "Artifacts" /
        importer.AssetPathToGUID("Packages/Models/LegacyReadOnlyHero.gltf") /
        "manifest.json";
    auto manifestJson = nlohmann::json::parse(ReadTextFile(manifestPath));
    manifestJson["importerVersion"] = 5u;
    WriteTextFile(manifestPath, manifestJson.dump(2));

    AssetDatabaseFacade readOnlyDatabase({
        {packageRoot, true, "Packages", root / "Library"}
    });
    ASSERT_TRUE(readOnlyDatabase.Refresh());

    EXPECT_FALSE(readOnlyDatabase.IsArtifactManifestCurrentForAssetPath("Packages/Models/LegacyReadOnlyHero.gltf"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ArtifactManifestCurrentRejectsTextureModelMissingTexturePipelineDependency)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroDiffuse.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.mtl",
        R"(
newmtl HeroMaterial
map_Kd ../Textures/HeroDiffuse.png
)");
    WriteTextFile(
        root / "Assets" / "Models" / "TexturePipelineHero.obj",
        R"(
mtllib Hero.mtl
o Hero
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl HeroMaterial
f 1/1/1 2/2/1 3/3/1
)");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/TexturePipelineHero.obj"));
    ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/TexturePipelineHero.obj"));

    const auto manifestPath =
        root / "Library" / "Artifacts" /
        database.AssetPathToGUID("Assets/Models/TexturePipelineHero.obj") /
        "manifest.json";
    auto manifestJson = nlohmann::json::parse(ReadTextFile(manifestPath));
    auto& dependencies = manifestJson["dependencies"];
    dependencies.erase(
        std::remove_if(
            dependencies.begin(),
            dependencies.end(),
            [](const nlohmann::json& dependency)
            {
                return dependency.value("kind", "") == "postprocessor-version" &&
                    dependency.value("value", "") == "external-texture-build-pipeline";
            }),
        dependencies.end());
    WriteTextFile(manifestPath, manifestJson.dump(2));

    AssetDatabaseFacade restartedDatabase({root});
    ASSERT_TRUE(restartedDatabase.Refresh());
    EXPECT_FALSE(restartedDatabase.IsArtifactManifestCurrentForAssetPath("Assets/Models/TexturePipelineHero.obj"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedModelManifestRecordsExternalSourceDependencies)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.bin", "mesh-binary");
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "uri": "Hero.bin", "byteLength": 11 }
            ],
            "images": [
                { "uri": "../Textures/HeroBaseColor.png", "mimeType": "image/png" }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "textures": [
                { "source": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    const auto artifactRoot =
        root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Models/Hero.gltf");
    {
        std::ifstream input(artifactRoot / "manifest.json", std::ios::binary);
        ASSERT_TRUE(input.good());
        const auto manifest = nlohmann::json::parse(input, nullptr, false);
        ASSERT_TRUE(manifest.is_object());

        EXPECT_TRUE(ContainsManifestDependency(manifest, "source-file-hash", "Assets/Models/Hero.gltf"));
        EXPECT_TRUE(ContainsManifestDependency(manifest, "source-file-hash", "Assets/Models/Hero.bin"));
        EXPECT_TRUE(ContainsManifestDependency(manifest, "source-file-hash", "Assets/Textures/HeroBaseColor.png"));
    }

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedObjManifestRecordsMtlAndTextureDependencies)
{
    using namespace NLS::Editor::Assets;

    EnsureAssetDatabaseFacadeTestDriver();
    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroDiffuse.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroNormal.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.mtl",
        R"(
newmtl HeroMaterial
Kd 1.0 1.0 1.0
map_Kd -s 1 1 1 ../Textures/HeroDiffuse.png
)");
    WriteTextFile(
        root / "Assets" / "Models" / "HeroExtra.mtl",
        R"(
newmtl HeroMaterialExtra
map_Bump ../Textures/HeroNormal.png
)");
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.obj",
        R"(
mtllib Hero.mtl HeroExtra.mtl
o Hero
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl HeroMaterial
f 1/1/1 2/2/1 3/3/1
)");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.obj"));

    const auto artifactRoot =
        root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Models/Hero.obj");
    {
        std::ifstream input(artifactRoot / "manifest.json", std::ios::binary);
        ASSERT_TRUE(input.good());
        const auto manifest = nlohmann::json::parse(input, nullptr, false);
        ASSERT_TRUE(manifest.is_object());

        EXPECT_TRUE(ContainsManifestDependency(manifest, "source-file-hash", "Assets/Models/Hero.obj"));
        EXPECT_TRUE(ContainsManifestDependency(manifest, "source-file-hash", "Assets/Models/Hero.mtl"));
        EXPECT_TRUE(ContainsManifestDependency(manifest, "source-file-hash", "Assets/Models/HeroExtra.mtl"));
        EXPECT_TRUE(ContainsManifestDependency(manifest, "source-file-hash", "Assets/Textures/HeroDiffuse.png"));
        EXPECT_TRUE(ContainsManifestDependency(manifest, "source-file-hash", "Assets/Textures/HeroNormal.png"));
    }

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedAssimpModelManifestRecordsParserTextureDependencies)
{
#if !NLS_HAS_AUTODESK_FBX_SDK
    GTEST_SKIP() << "FBX import success requires Autodesk FBX SDK.";
#endif

    using namespace NLS::Editor::Assets;

    EnsureAssetDatabaseFacadeTestDriver();
    const auto sourceRoot =
        std::filesystem::current_path() /
        "ThirdParty" / "assimp" / "test" / "models-nonbsd" / "FBX" / "2013_ASCII";
    const auto sourceFbx = sourceRoot / "jeep1.fbx";
    const auto sourceTexture = sourceRoot / "jeep1.jpg";
    ASSERT_TRUE(std::filesystem::exists(sourceFbx));
    ASSERT_TRUE(std::filesystem::exists(sourceTexture));

    const auto root = MakeAssetDatabaseFacadeRoot();
    std::filesystem::create_directories(root / "Assets" / "Models");
    std::filesystem::copy_file(
        sourceFbx,
        root / "Assets" / "Models" / "jeep1.fbx",
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(
        sourceTexture,
        root / "Assets" / "Models" / "jeep1.jpg",
        std::filesystem::copy_options::overwrite_existing);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ImportProgressTracker tracker;
    ASSERT_TRUE(database.ImportAsset("Assets/Models/jeep1.fbx", tracker));

    bool reportedSecondSourceMeshBuild = false;
    for (const auto& event : tracker.GetEvents({1u}))
    {
        if (event.message == "Building native mesh cache")
            reportedSecondSourceMeshBuild = true;
    }
    EXPECT_FALSE(reportedSecondSourceMeshBuild);

    const auto artifactRoot =
        root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Models/jeep1.fbx");
    {
        std::ifstream input(artifactRoot / "manifest.json", std::ios::binary);
        ASSERT_TRUE(input.good());
        const auto manifest = nlohmann::json::parse(input, nullptr, false);
        ASSERT_TRUE(manifest.is_object());

        EXPECT_TRUE(ContainsManifestDependency(manifest, "source-file-hash", "Assets/Models/jeep1.fbx"));
        EXPECT_TRUE(ContainsManifestDependency(manifest, "source-file-hash", "Assets/Models/jeep1.jpg"));
    }

    const auto allAssets = database.LoadAllAssetsAtPath("Assets/Models/jeep1.fbx");
    const auto meshAsset = std::find_if(
        allAssets.begin(),
        allAssets.end(),
        [](const AssetDatabaseRecord& asset)
        {
            return asset.artifactType == NLS::Core::Assets::ArtifactType::Mesh;
        });
    ASSERT_NE(meshAsset, allAssets.end());
    const auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshAsset->artifactPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_GT(meshArtifact->vertices.size(), 0u);
    EXPECT_GT(meshArtifact->indices.size(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedAssimpModelImportDoesNotCommitEmptyArtifacts)
{
    using namespace NLS::Editor::Assets;

    EnsureAssetDatabaseFacadeTestDriver();
    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Broken.fbx", "not a valid fbx model");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ImportProgressTracker tracker;
    EXPECT_FALSE(database.ImportAsset("Assets/Models/Broken.fbx", tracker));

    const auto artifactRoot =
        root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Models/Broken.fbx");
    EXPECT_FALSE(std::filesystem::exists(artifactRoot / "manifest.json"));
    EXPECT_FALSE(database.GetArtifactManifestForAssetPath("Assets/Models/Broken.fbx").has_value());

    bool reportedFailure = false;
    for (const auto& event : tracker.GetEvents({1u}))
    {
        if (event.terminalStatus == ImportJobTerminalStatus::Failed)
            reportedFailure = true;
    }
    EXPECT_TRUE(reportedFailure);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedModelMeshArtifactMergesMultiplePrimitives)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "TwoTriangles.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAAAAAAAAAIA/AAAAPwAAgD8AAAAAAAAAPwAAAAAAAIA/AAABAAIA",
                    "byteLength": 84
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 },
                { "buffer": 0, "byteOffset": 42, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 78, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" },
                { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "Double",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 },
                        { "attributes": { "POSITION": 2 }, "indices": 3 }
                    ]
                }
            ],
            "nodes": [
                { "name": "DoubleRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/TwoTriangles.gltf"));

    const auto artifactRoot = root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Models/TwoTriangles.gltf");
    const auto firstPrimitiveArtifact = NLS::Render::Assets::LoadMeshArtifact(
        artifactRoot / "meshes" / "mesh%3Amesh%2F0%2Fprimitive%2F0.nmesh");
    const auto secondPrimitiveArtifact = NLS::Render::Assets::LoadMeshArtifact(
        artifactRoot / "meshes" / "mesh%3Amesh%2F0%2Fprimitive%2F1.nmesh");

    ASSERT_TRUE(firstPrimitiveArtifact.has_value());
    ASSERT_TRUE(secondPrimitiveArtifact.has_value());
    EXPECT_EQ(firstPrimitiveArtifact->vertices.size(), 3u);
    EXPECT_EQ(firstPrimitiveArtifact->indices.size(), 3u);
    EXPECT_EQ(secondPrimitiveArtifact->vertices.size(), 3u);
    EXPECT_EQ(secondPrimitiveArtifact->indices.size(), 3u);
    EXPECT_EQ(secondPrimitiveArtifact->indices[0], 0u);
    EXPECT_EQ(secondPrimitiveArtifact->indices[1], 1u);
    EXPECT_EQ(secondPrimitiveArtifact->indices[2], 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ReimportAssetRefreshesStaleNativeMeshArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Models" / "Reimported.gltf";
    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "Single",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 }
                    ]
                }
            ],
            "nodes": [
                { "name": "SingleRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Reimported.gltf"));
    const auto artifactRoot = root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Models/Reimported.gltf");
    const auto meshPath = artifactRoot / "meshes" / "mesh%3Amesh%2F0.nmesh";
    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);

    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAAAAAAAAAIA/AAAAPwAAgD8AAAAAAAAAPwAAAAAAAIA/AAABAAIA",
                    "byteLength": 84
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 },
                { "buffer": 0, "byteOffset": 42, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 78, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" },
                { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "Double",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 },
                        { "attributes": { "POSITION": 2 }, "indices": 3 }
                    ]
                }
            ],
            "nodes": [
                { "name": "DoubleRoot", "mesh": 0 }
            ]
        })");

    ASSERT_TRUE(database.ReimportAsset("Assets/Models/Reimported.gltf"));
    const auto firstPrimitivePath = artifactRoot / "meshes" / "mesh%3Amesh%2F0%2Fprimitive%2F0.nmesh";
    const auto secondPrimitivePath = artifactRoot / "meshes" / "mesh%3Amesh%2F0%2Fprimitive%2F1.nmesh";
    EXPECT_FALSE(std::filesystem::exists(meshPath));

    const auto firstPrimitiveArtifact = NLS::Render::Assets::LoadMeshArtifact(firstPrimitivePath);
    const auto secondPrimitiveArtifact = NLS::Render::Assets::LoadMeshArtifact(secondPrimitivePath);
    ASSERT_TRUE(firstPrimitiveArtifact.has_value());
    ASSERT_TRUE(secondPrimitiveArtifact.has_value());
    EXPECT_EQ(firstPrimitiveArtifact->vertices.size(), 3u);
    EXPECT_EQ(firstPrimitiveArtifact->indices.size(), 3u);
    EXPECT_EQ(secondPrimitiveArtifact->vertices.size(), 3u);
    EXPECT_EQ(secondPrimitiveArtifact->indices.size(), 3u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ReimportAssetRefreshesNativeTextureArtifactsAndCentralIndex)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Models" / "Textured.gltf";
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "images": [
                { "uri": "../Textures/HeroBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "meshes": [
                {
                    "name": "HeroMesh",
                    "primitives": [
                        { "attributes": {}, "material": 0 }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Textured.gltf"));
    ASSERT_TRUE(database.ReimportAsset("Assets/Models/Textured.gltf"));

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB" / "index.tsv"));
    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Textured.gltf"));
    const auto* textureRecord = artifactDatabase.Find(sourceId, "texture:image/0", "win64-dx12");
    if (textureRecord == nullptr)
        textureRecord = artifactDatabase.Find(sourceId, "texture:image/0", "editor");
    ASSERT_NE(textureRecord, nullptr);
    EXPECT_EQ(textureRecord->artifactType, ArtifactType::Texture);
    EXPECT_EQ(textureRecord->loaderId, "texture");
    EXPECT_FALSE(std::filesystem::path(textureRecord->artifactPath).is_absolute());
    EXPECT_EQ(textureRecord->artifactPath.find("Library/Artifacts/" + sourceId.ToString() + "/"), 0u)
        << textureRecord->artifactPath;

    const auto texturePath = root / textureRecord->artifactPath;
    EXPECT_TRUE(std::filesystem::exists(texturePath));

    const auto textureSubAsset = database.LoadSubAssetAtPath("Assets/Models/Textured.gltf", "texture:image/0");
    ASSERT_TRUE(textureSubAsset.has_value());
    EXPECT_EQ(textureSubAsset->artifactType, ArtifactType::Texture);
    EXPECT_TRUE(std::filesystem::equivalent(textureSubAsset->artifactPath, texturePath));

    const auto artifact = NLS::Render::Assets::LoadTextureArtifact(texturePath);
    ASSERT_TRUE(artifact.has_value());
    ASSERT_FALSE(artifact->mips.empty());
    EXPECT_EQ(artifact->width, 1u);
    EXPECT_EQ(artifact->height, 1u);
    EXPECT_FALSE(artifact->targetPlatform.empty());
    EXPECT_FALSE(artifact->encoderId.empty());

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedAssetDatabaseFacadeDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TextureReimportTestDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::LINEAR,
        NLS::Render::Settings::ETextureFilteringMode::LINEAR,
        false);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->width, artifact->width);
    EXPECT_EQ(texture->height, artifact->height);
    EXPECT_EQ(texture->isMimapped, artifact->mips.size() > 1u);
    EXPECT_EQ(texture->path, texturePath.string());
    EXPECT_GE(explicitDevice->textureCreateCalls, 1u);
    EXPECT_EQ(explicitDevice->lastTextureDesc.format, artifact->format);
    EXPECT_EQ(explicitDevice->lastTextureDesc.mipLevels, artifact->mips.size());
    EXPECT_GT(explicitDevice->lastTextureUploadDesc.dataSize, 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedReimportKeepsPreviousNativeMeshArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Models" / "Stable.gltf";
    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "StableMesh",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 }
                    ]
                }
            ],
            "nodes": [
                { "name": "StableRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Stable.gltf"));
    const auto artifactRoot = root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Models/Stable.gltf");
    const auto meshPath = artifactRoot / "meshes" / "mesh%3Amesh%2F0.nmesh";
    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);

    std::filesystem::remove(assetPath);

    EXPECT_FALSE(database.ReimportAsset("Assets/Models/Stable.gltf"));
    meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedReimportRollsBackCommittedArtifactsWhenManifestCannotBeSaved)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Models" / "Transactional.gltf";
    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "StableMesh",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 }
                    ]
                }
            ],
            "nodes": [
                { "name": "StableRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Transactional.gltf"));
    const auto artifactRoot = root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Models/Transactional.gltf");
    const auto meshPath = artifactRoot / "meshes" / "mesh%3Amesh%2F0.nmesh";
    const auto manifestPath = artifactRoot / "manifest.json";
    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    ASSERT_EQ(meshArtifact->vertices.size(), 3u);
    const auto originalMeshBytes = std::filesystem::file_size(meshPath);
    const auto originalManifestBytes = std::filesystem::file_size(manifestPath);

    WriteTextFile(
        assetPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAAAAAAAAAAAAIA/AAAAPwAAgD8AAAAAAAAAPwAAAAAAAIA/AAABAAIA",
                    "byteLength": 84
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 },
                { "buffer": 0, "byteOffset": 42, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 78, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" },
                { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "DoubleMesh",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 },
                        { "attributes": { "POSITION": 2 }, "indices": 3 }
                    ]
                }
            ],
            "nodes": [
                { "name": "DoubleRoot", "mesh": 0 }
            ]
        })");

    std::filesystem::remove(manifestPath);
    std::filesystem::create_directory(manifestPath);

    EXPECT_FALSE(database.ReimportAsset("Assets/Models/Transactional.gltf"));
    EXPECT_TRUE(std::filesystem::is_directory(manifestPath));
    EXPECT_EQ(std::filesystem::file_size(meshPath), originalMeshBytes);

    meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);
    EXPECT_EQ(meshArtifact->indices.size(), 3u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FailedPrefabReimportRollsBackCommittedPayloadWhenManifestCannotBeSaved)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Prefabs" / "Transactional.prefab";
    NLS::Engine::GameObject stable("Stable", "Prefab");
    auto stablePrefab = PrefabEditorWorkflow().CreatePrefabFromSelection({
        &stable,
        {},
        ParseAssetId("11111111-1111-4111-8111-111111111111"),
        "Assets/Prefabs/Transactional.prefab"
    });
    ASSERT_EQ(stablePrefab.status, PrefabEditorOperationStatus::Committed);
    WriteTextFile(assetPath, stablePrefab.prefabSourceText);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/Transactional.prefab"));

    const auto artifactRoot = root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Prefabs/Transactional.prefab");
    const auto prefabPayloadPath = artifactRoot / "prefab.nprefab";
    const auto manifestPath = artifactRoot / "manifest.json";
    ASSERT_TRUE(std::filesystem::exists(prefabPayloadPath));
    ASSERT_TRUE(std::filesystem::exists(manifestPath));
    const auto originalPayloadBytes = std::filesystem::file_size(prefabPayloadPath);

    NLS::Engine::GameObject changed("ChangedWithLongerPayload", "UpdatedPrefabTag");
    auto changedPrefab = PrefabEditorWorkflow().CreatePrefabFromSelection({
        &changed,
        {},
        ParseAssetId("22222222-2222-4222-8222-222222222222"),
        "Assets/Prefabs/Transactional.prefab"
    });
    ASSERT_EQ(changedPrefab.status, PrefabEditorOperationStatus::Committed);
    WriteTextFile(assetPath, changedPrefab.prefabSourceText);
    std::filesystem::remove(manifestPath);
    std::filesystem::create_directory(manifestPath);

    EXPECT_FALSE(database.ReimportAsset("Assets/Prefabs/Transactional.prefab"));
    EXPECT_TRUE(std::filesystem::is_directory(manifestPath));
    EXPECT_EQ(std::filesystem::file_size(prefabPayloadPath), originalPayloadBytes);

    auto prefab = database.LoadPrefabArtifactAtPath(
        "Assets/Prefabs/Transactional.prefab",
        "prefab:Transactional");
    EXPECT_FALSE(prefab.has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RefreshClearsWarmPrefabStateWhenPersistedManifestCannotBeRead)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto assetPath = root / "Assets" / "Prefabs" / "BrokenManifest.prefab";
    NLS::Engine::GameObject stable("Stable", "Prefab");
    auto stablePrefab = PrefabEditorWorkflow().CreatePrefabFromSelection({
        &stable,
        {},
        ParseAssetId("33333333-3333-4333-8333-333333333333"),
        "Assets/Prefabs/BrokenManifest.prefab"
    });
    ASSERT_EQ(stablePrefab.status, PrefabEditorOperationStatus::Committed);
    WriteTextFile(assetPath, stablePrefab.prefabSourceText);

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/BrokenManifest.prefab"));
    ASSERT_TRUE(database
        .LoadPrefabArtifactAtPath("Assets/Prefabs/BrokenManifest.prefab", "prefab:BrokenManifest")
        .has_value());

    const auto artifactRoot = root / "Library" / "Artifacts" / database.AssetPathToGUID("Assets/Prefabs/BrokenManifest.prefab");
    const auto manifestPath = artifactRoot / "manifest.json";
    std::filesystem::remove(manifestPath);
    std::filesystem::create_directory(manifestPath);

    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database
        .LoadPrefabArtifactAtPath("Assets/Prefabs/BrokenManifest.prefab", "prefab:BrokenManifest")
        .has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportsSameStemGltfAndFbxIntoSeparateGuidArtifactRoots)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Sponza.gltf",
        R"({
            "asset": { "version": "2.0" },
            "meshes": [
                {
                    "name": "GltfBody",
                    "primitives": [
                        { "attributes": { "POSITION": 0 } }
                    ]
                }
            ],
            "nodes": [
                { "name": "SponzaGltfRoot", "mesh": 0 }
            ]
        })");
    WriteTextFile(root / "Assets" / "Models" / "Sponza.fbx", "placeholder fbx");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto gltfGuid = database.AssetPathToGUID("Assets/Models/Sponza.gltf");
    const auto fbxGuid = database.AssetPathToGUID("Assets/Models/Sponza.fbx");
    ASSERT_FALSE(gltfGuid.empty());
    ASSERT_FALSE(fbxGuid.empty());
    ASSERT_NE(gltfGuid, fbxGuid);

    const auto gltfRoot = database.GetArtifactRootForAssetPathForTesting("Assets/Models/Sponza.gltf");
    const auto fbxRoot = database.GetArtifactRootForAssetPathForTesting("Assets/Models/Sponza.fbx");
    EXPECT_EQ(gltfRoot, root / "Library" / "Artifacts" / gltfGuid);
    EXPECT_EQ(fbxRoot, root / "Library" / "Artifacts" / fbxGuid);
    EXPECT_NE(gltfRoot, fbxRoot);
    EXPECT_NE(gltfRoot, root / "Library" / "Artifacts" / "Sponza");
    EXPECT_NE(fbxRoot, root / "Library" / "Artifacts" / "Sponza");

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, AssetBrowserHidesImportedModelGeneratedPrefabSubAsset)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        { "attributes": { "POSITION": 0 } }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    {
        AssetDatabaseFacade importer({root});
        ASSERT_TRUE(importer.Refresh());
        ASSERT_TRUE(importer.ImportAsset("Assets/Models/Hero.gltf"));
    }

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto entries = BuildAssetBrowserSubAssetEntries(database, "Assets/Models/Hero.gltf");
    EXPECT_TRUE(std::none_of(
        entries.begin(),
        entries.end(),
        [](const AssetBrowserSubAssetEntry& entry)
        {
            return entry.artifactType == ArtifactType::Prefab || entry.subAssetKey.rfind("prefab:", 0) == 0;
        }));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, AssetBrowserExposesImportedModelReferenceableSubAssetsForInspectorDrag)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = modelId;
    manifest.importerId = "scene-model";
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "model:Hero";
    manifest.subAssets.push_back(MakeArtifact(modelId, "model:Hero", ArtifactType::Model, "model"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "prefab:Hero", ArtifactType::Prefab, "prefab"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "mesh:Body", ArtifactType::Mesh, "mesh"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "material:Body", ArtifactType::Material, "material"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "texture:Albedo", ArtifactType::Texture, "texture"));
    manifest.subAssets.push_back(MakeArtifact(modelId, "animation:Idle", ArtifactType::AnimationClip, "animation"));
    database.AddArtifactManifest(manifest);

    const auto entries = BuildAssetBrowserSubAssetEntries(database, "Assets/Models/Hero.gltf");
    ASSERT_EQ(entries.size(), 3u);

    EXPECT_EQ(entries[0].displayName, "Body");
    EXPECT_EQ(entries[0].sourceAssetPath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(entries[0].subAssetKey, "mesh:Body");
    EXPECT_EQ(entries[0].dragResourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(entries[0].assetId, modelId);
    EXPECT_EQ(entries[0].artifactType, ArtifactType::Mesh);
    EXPECT_TRUE(entries[0].generatedReadOnly);

    EXPECT_EQ(entries[1].displayName, "Body");
    EXPECT_EQ(entries[1].subAssetKey, "material:Body");
    EXPECT_EQ(entries[1].artifactType, ArtifactType::Material);

    EXPECT_EQ(entries[2].displayName, "Albedo");
    EXPECT_EQ(entries[2].subAssetKey, "texture:Albedo");
    EXPECT_EQ(entries[2].artifactType, ArtifactType::Texture);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportedModelGeneratedPrefabLoadsAndInstantiatesThroughDragDropWorkflow)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorFactor": [1.0, 0.2, 0.1, 1.0]
                    }
                }
            ],
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "HeroRoot", "mesh": 0 }
            ]
        })");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    auto prefab = database.LoadPrefabArtifactAtPath("Assets/Models/Hero.gltf", "prefab:Hero");
    ASSERT_TRUE(prefab.has_value());
    EXPECT_TRUE(prefab->generatedModelPrefab);
    EXPECT_FALSE(prefab->Validate().HasErrors());

    NLS::Engine::SceneSystem::Scene scene;
    AssetDragDropWorkflow workflow;
    const auto sceneId = ParseAssetId("e4040404-0404-4404-8404-040404040404");
    const auto result = workflow.Execute({
        {DragPayloadKind::GeneratedModelPrefabAsset, prefab->assetId, "prefab:Hero", &*prefab},
        {DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        sceneId
    });

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    ASSERT_NE(result.instance->instanceRoot, nullptr);
    EXPECT_EQ(result.instance->instanceRoot->GetName(), "HeroRoot");
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front(), result.instance->instanceRoot);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, EditorDragDropBridgeInstantiatesPreimportedModelGeneratedPrefab)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const std::string bridgeHeroGltf = R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "BridgeHeroRoot", "mesh": 0 }
            ]
        })";
    WriteTextFile(
        root / "Assets" / "Models" / "BridgeHero.gltf",
        bridgeHeroGltf);

    NLS::Engine::SceneSystem::Scene scene;
    EditorAssetDragDropBridge bridge(root / "Assets");
    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Models/BridgeHero.gltf"));
    }
    const auto result = bridge.DropModelAssetIntoHierarchy("Models/BridgeHero.gltf", scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "BridgeHeroRoot");
    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    const auto artifactRoot = database.GetArtifactRootForAssetPathForTesting("Assets/Models/BridgeHero.gltf");
    EXPECT_TRUE(std::filesystem::exists(artifactRoot / "prefab.nprefab"));
    EXPECT_TRUE(std::filesystem::exists(artifactRoot / "manifest.json"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, EditorDragDropBridgeInstantiatesGeneratedPrefabSubAssetResource)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const std::string bridgeHeroGltf = R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "BridgeHeroRoot", "mesh": 0 }
            ]
        })";
    WriteTextFile(
        root / "Assets" / "Models" / "BridgeHero.gltf",
        bridgeHeroGltf);

    {
        AssetDatabaseFacade importer({root});
        ASSERT_TRUE(importer.Refresh());
        ASSERT_TRUE(importer.ImportAsset("Assets/Models/BridgeHero.gltf"));
    }

    NLS::Engine::SceneSystem::Scene scene;
    EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropModelAssetIntoHierarchy(
        "Assets/Models/BridgeHero.gltf#prefab:BridgeHero.prefab",
        scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "BridgeHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, EditorDragDropBridgeInstantiatesPreimportedPrefabSource)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    NLS::Engine::GameObject gameObject("Lamp", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        ParseAssetId("e7070707-0707-4707-8707-070707070707"),
        "Assets/Prefabs/Lamp.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);

    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.CreateTextAsset(
            created.prefabSourceText,
            "Assets/Prefabs/Lamp.prefab",
            ParseAssetId("e8080808-0808-4808-8808-080808080808")));
    }

    NLS::Engine::SceneSystem::Scene scene;
    EditorAssetDragDropBridge bridge(root / "Assets");
    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/Lamp.prefab"));
    }
    const auto result = bridge.DropModelAssetIntoHierarchy("Assets/Prefabs/Lamp.prefab", scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "Lamp");

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, DependencyQueriesReturnDirectAndRecursiveAssetPaths)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Hero.prefab", "{}");
    WriteTextFile(root / "Assets" / "Materials" / "Body.mat", "material");
    WriteTextFile(root / "Assets" / "Textures" / "Body.png", "texture");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto prefabId = ParseAssetId(database.AssetPathToGUID("Assets/Prefabs/Hero.prefab"));
    const auto materialId = ParseAssetId(database.AssetPathToGUID("Assets/Materials/Body.mat"));
    const auto textureId = ParseAssetId(database.AssetPathToGUID("Assets/Textures/Body.png"));

    ArtifactManifest prefabManifest;
    prefabManifest.sourceAssetId = prefabId;
    prefabManifest.primarySubAssetKey = "prefab:Hero";
    prefabManifest.subAssets.push_back(MakeArtifact(prefabId, "prefab:Hero", ArtifactType::Prefab, "prefab", {}, {}, "win64"));
    prefabManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, materialId.ToString(), "material:Body"});

    ArtifactManifest materialManifest;
    materialManifest.sourceAssetId = materialId;
    materialManifest.primarySubAssetKey = "material:Body";
    materialManifest.subAssets.push_back(MakeArtifact(materialId, "material:Body", ArtifactType::Material, "material"));
    materialManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, textureId.ToString(), "texture:Body"});

    database.AddArtifactManifest(prefabManifest);
    database.AddArtifactManifest(materialManifest);

    EXPECT_EQ(
        database.GetDependencies("Assets/Prefabs/Hero.prefab", false),
        std::vector<std::string>({"Assets/Materials/Body.mat"}));

    EXPECT_EQ(
        database.GetDependencies("Assets/Prefabs/Hero.prefab", true),
        std::vector<std::string>({"Assets/Materials/Body.mat", "Assets/Textures/Body.png"}));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, SearchFiltersUseNameTypeLabelFolderAndDeterministicOrdering)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Characters" / "Hero.prefab", "{}");
    WriteTextFile(root / "Assets" / "Characters" / "Hero.mat", "material");
    WriteTextFile(root / "Assets" / "Environment" / "HeroRock.prefab", "{}");
    WriteTextFile(root / "Assets" / "Characters" / "Villain.prefab", "{}");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.SetLabels("Assets/Characters/Hero.prefab", {"character", "player"}));
    ASSERT_TRUE(database.SetLabels("Assets/Environment/HeroRock.prefab", {"environment"}));

    EXPECT_EQ(
        database.FindAssets("name:Hero type:prefab label:character", {"Assets/Characters"}),
        std::vector<std::string>({"Assets/Characters/Hero.prefab"}));

    EXPECT_EQ(
        database.FindAssets("type:prefab", {}),
        std::vector<std::string>({
            "Assets/Characters/Hero.prefab",
            "Assets/Characters/Villain.prefab",
            "Assets/Environment/HeroRock.prefab"
        }));

    EXPECT_EQ(
        database.GetLabels("Assets/Characters/Hero.prefab"),
        std::vector<std::string>({"character", "player"}));
    EXPECT_EQ(
        database.GetAllLabels(),
        std::vector<std::string>({"character", "environment", "player"}));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, BundleMetadataMapsToRuntimeAssetPacks)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Hero.prefab", "{}");
    WriteTextFile(root / "Assets" / "Materials" / "Hero.mat", "material");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.SetAssetPackNameAndVariant("Assets/Prefabs/Hero.prefab", "characters", "hd"));
    ASSERT_TRUE(database.SetAssetPackNameAndVariant("Assets/Materials/Hero.mat", "characters", "hd"));

    const auto prefabId = ParseAssetId(database.AssetPathToGUID("Assets/Prefabs/Hero.prefab"));
    const auto materialId = ParseAssetId(database.AssetPathToGUID("Assets/Materials/Hero.mat"));

    ArtifactManifest prefabManifest;
    prefabManifest.sourceAssetId = prefabId;
    prefabManifest.targetPlatform = "win64";
    prefabManifest.primarySubAssetKey = "prefab:Hero";
    prefabManifest.subAssets.push_back(MakeArtifact(prefabId, "prefab:Hero", ArtifactType::Prefab, "prefab", {}, {}, "win64"));

    ArtifactManifest materialManifest;
    materialManifest.sourceAssetId = materialId;
    materialManifest.targetPlatform = "win64";
    materialManifest.primarySubAssetKey = "material:Hero";
    materialManifest.subAssets.push_back(MakeArtifact(materialId, "material:Hero", ArtifactType::Material, "material", {}, {}, "win64"));

    database.AddArtifactManifest(prefabManifest);
    database.AddArtifactManifest(materialManifest);

    const auto packInfo = database.GetAssetPackNameAndVariant("Assets/Prefabs/Hero.prefab");
    ASSERT_TRUE(packInfo.has_value());
    EXPECT_EQ(packInfo->name, "characters");
    EXPECT_EQ(packInfo->variant, "hd");

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(prefabManifest);
    builder.AddArtifactManifest(materialManifest);
    const auto result = builder.BuildAssetPacks(database.GetAssetPackBuildInputs(), "win64");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.manifest.assetPacks.size(), 1u);
    EXPECT_EQ(result.manifest.assetPacks[0].packName, "characters");
    EXPECT_EQ(result.manifest.assetPacks[0].packVariant, "hd");
    EXPECT_EQ(result.manifest.assetPacks[0].entries.size(), 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, AssetPacksIncludeDependencyClosureLoaderHashesAndVariants)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Hero.prefab", "{}");
    WriteTextFile(root / "Assets" / "Prefabs" / "Villain.prefab", "{}");
    WriteTextFile(root / "Assets" / "Materials" / "Body.mat", "material");
    WriteTextFile(root / "Assets" / "Textures" / "Body.png", "texture");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.SetAssetPackNameAndVariant("Assets/Prefabs/Hero.prefab", "characters", "hd"));
    ASSERT_TRUE(database.SetAssetPackNameAndVariant("Assets/Materials/Body.mat", "characters", "hd"));
    ASSERT_TRUE(database.SetAssetPackNameAndVariant("Assets/Prefabs/Villain.prefab", "characters", "sd"));

    const auto prefabId = ParseAssetId(database.AssetPathToGUID("Assets/Prefabs/Hero.prefab"));
    const auto villainId = ParseAssetId(database.AssetPathToGUID("Assets/Prefabs/Villain.prefab"));
    const auto materialId = ParseAssetId(database.AssetPathToGUID("Assets/Materials/Body.mat"));
    const auto textureId = ParseAssetId(database.AssetPathToGUID("Assets/Textures/Body.png"));

    ArtifactManifest prefabManifest;
    prefabManifest.sourceAssetId = prefabId;
    prefabManifest.targetPlatform = "win64";
    prefabManifest.primarySubAssetKey = "prefab:Hero";
    prefabManifest.subAssets.push_back(MakeArtifact(
        prefabId,
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Artifacts/Hero/Hero.nprefab",
        "sha256:hero-prefab",
        "win64"));
    prefabManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, materialId.ToString(), "material:Body"});

    ArtifactManifest materialManifest;
    materialManifest.sourceAssetId = materialId;
    materialManifest.targetPlatform = "win64";
    materialManifest.primarySubAssetKey = "material:Body";
    materialManifest.subAssets.push_back(MakeArtifact(
        materialId,
        "material:Body",
        ArtifactType::Material,
        "material",
        "Artifacts/Hero/Body.nmat",
        "sha256:body-material",
        "win64"));
    materialManifest.dependencies.push_back({AssetDependencyKind::ImportedArtifact, textureId.ToString(), "texture:Body"});

    ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = textureId;
    textureManifest.targetPlatform = "win64";
    textureManifest.primarySubAssetKey = "texture:Body";
    textureManifest.subAssets.push_back(MakeArtifact(
        textureId,
        "texture:Body",
        ArtifactType::Texture,
        "texture",
        "Artifacts/Hero/Body.ntex",
        "sha256:body-texture",
        "win64"));

    ArtifactManifest villainManifest;
    villainManifest.sourceAssetId = villainId;
    villainManifest.targetPlatform = "win64";
    villainManifest.primarySubAssetKey = "prefab:Villain";
    villainManifest.subAssets.push_back(MakeArtifact(
        villainId,
        "prefab:Villain",
        ArtifactType::Prefab,
        "prefab",
        "Artifacts/Villain/Villain.nprefab",
        "sha256:villain-prefab",
        "win64"));

    database.AddArtifactManifest(prefabManifest);
    database.AddArtifactManifest(materialManifest);
    database.AddArtifactManifest(textureManifest);
    database.AddArtifactManifest(villainManifest);

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(prefabManifest);
    builder.AddArtifactManifest(materialManifest);
    builder.AddArtifactManifest(textureManifest);
    builder.AddArtifactManifest(villainManifest);

    const auto result = builder.BuildAssetPacks(database.GetAssetPackBuildInputs(), "win64");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.manifest.assetPacks.size(), 2u);

    const auto* hdPack = FindPack(result.manifest, "characters", "hd");
    ASSERT_NE(hdPack, nullptr);
    ASSERT_EQ(hdPack->entries.size(), 3u);

    const auto* prefabEntry = FindPackEntry(*hdPack, prefabId, "prefab:Hero");
    ASSERT_NE(prefabEntry, nullptr);
    EXPECT_EQ(prefabEntry->artifactType, ArtifactType::Prefab);
    EXPECT_EQ(prefabEntry->loaderId, "prefab");
    EXPECT_EQ(prefabEntry->artifactPath, "Artifacts/Hero/Hero.nprefab");
    EXPECT_EQ(prefabEntry->contentHash, "sha256:hero-prefab");
    EXPECT_TRUE(ContainsDependency(prefabEntry->dependencies, materialId, "material:Body"));

    const auto* materialEntry = FindPackEntry(*hdPack, materialId, "material:Body");
    ASSERT_NE(materialEntry, nullptr);
    EXPECT_EQ(materialEntry->loaderId, "material");
    EXPECT_EQ(materialEntry->contentHash, "sha256:body-material");
    EXPECT_TRUE(ContainsDependency(materialEntry->dependencies, textureId, "texture:Body"));

    const auto* textureEntry = FindPackEntry(*hdPack, textureId, "texture:Body");
    ASSERT_NE(textureEntry, nullptr);
    EXPECT_EQ(textureEntry->loaderId, "texture");
    EXPECT_EQ(textureEntry->contentHash, "sha256:body-texture");
    EXPECT_TRUE(textureEntry->dependencies.empty());

    const auto* sdPack = FindPack(result.manifest, "characters", "sd");
    ASSERT_NE(sdPack, nullptr);
    ASSERT_EQ(sdPack->entries.size(), 1u);
    EXPECT_NE(FindPackEntry(*sdPack, villainId, "prefab:Villain"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RuntimeLoadsPackagedAssetsFromManifestAndRejectsEditorOnlyApis)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Hero.prefab", "{}");

    const auto prefabId = NLS::Core::Assets::AssetId::New();
    RuntimeAssetManifest manifest;
    manifest.targetPlatform = "win64";
    manifest.entries.push_back({
        prefabId,
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Artifacts/Hero/Hero.nprefab",
        "sha256:hero-prefab",
        {}
    });

    RuntimeAssetDatabase runtimeDatabase(manifest);
    const auto* entry = runtimeDatabase.Resolve({prefabId, "prefab:Hero"});
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->loaderId, "prefab");
    EXPECT_EQ(entry->contentHash, "sha256:hero-prefab");
    EXPECT_TRUE(IsRuntimePackagedAssetPath(entry->artifactPath));

    EXPECT_TRUE(IsRuntimeAssetApiAvailable("RuntimeAssetDatabase.Resolve"));
    EXPECT_FALSE(IsRuntimeAssetApiAvailable("AssetDatabase.Refresh"));
    EXPECT_FALSE(IsRuntimeAssetApiAvailable("AssetDatabase.ImportAsset"));
    EXPECT_FALSE(IsRuntimeAssetApiAvailable("AssetImporter.SaveAndReimport"));
    EXPECT_FALSE(IsRuntimeAssetApiAvailable("ModelImporter.SaveAndReimport"));
    EXPECT_FALSE(IsRuntimeAssetApiAvailable("TextureImporter.SaveAndReimport"));

    AssetDatabaseFacade runtimeFacade({root}, AssetDatabaseAccessMode::Runtime);
    EXPECT_FALSE(runtimeFacade.Refresh());
    EXPECT_FALSE(runtimeFacade.ImportAsset("Assets/Prefabs/Hero.prefab"));
    EXPECT_TRUE(runtimeFacade.AssetPathToGUID("Assets/Prefabs/Hero.prefab").empty());
    EXPECT_FALSE(runtimeFacade.SetAssetPackNameAndVariant("Assets/Prefabs/Hero.prefab", "characters", "hd"));
    EXPECT_TRUE(ContainsAssetDiagnosticCode(
        runtimeFacade.GetDiagnostics(),
        "assetdatabase-editor-api-unavailable-at-runtime"));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, CreateAddExtractAndContainmentUseAssetObjectSemantics)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    AssetObjectRecord material;
    material.name = "HeroMaterial";
    material.artifactType = ArtifactType::Material;
    material.loaderId = "material";
    material.serializedPayload = "albedo=1,1,1,1";

    ASSERT_TRUE(database.CreateAsset(material, "Assets/Materials/Hero.mat"));
    ASSERT_TRUE(std::filesystem::exists(root / "Assets" / "Materials" / "Hero.mat"));
    const auto materialGuid = database.AssetPathToGUID("Assets/Materials/Hero.mat");
    ASSERT_FALSE(materialGuid.empty());

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Materials/Hero.mat");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->artifactType, ArtifactType::Material);
    EXPECT_TRUE(database.Contains(*mainAsset));
    EXPECT_TRUE(database.IsMainAsset(*mainAsset));
    EXPECT_FALSE(database.IsSubAsset(*mainAsset));

    AssetObjectRecord embeddedTexture;
    embeddedTexture.name = "EmbeddedMask";
    embeddedTexture.artifactType = ArtifactType::Texture;
    embeddedTexture.loaderId = "texture";
    embeddedTexture.serializedPayload = "mask-bytes";

    ASSERT_TRUE(database.AddObjectToAsset(embeddedTexture, "Assets/Materials/Hero.mat"));
    auto allAssets = database.LoadAllAssetsAtPath("Assets/Materials/Hero.mat");
    ASSERT_EQ(allAssets.size(), 2u);
    const auto subAsset = database.LoadSubAssetAtPath("Assets/Materials/Hero.mat", "texture:EmbeddedMask");
    ASSERT_TRUE(subAsset.has_value());
    EXPECT_TRUE(database.Contains(*subAsset));
    EXPECT_FALSE(database.IsMainAsset(*subAsset));
    EXPECT_TRUE(database.IsSubAsset(*subAsset));

    const auto uniquePath = database.GenerateUniqueAssetPath("Assets/Materials/Hero.mat");
    EXPECT_EQ(uniquePath, "Assets/Materials/Hero 1.mat");

    ASSERT_TRUE(database.ExtractAsset(*subAsset, "Assets/Textures/ExtractedMask.ntex"));
    EXPECT_FALSE(database.LoadSubAssetAtPath("Assets/Materials/Hero.mat", "texture:EmbeddedMask").has_value());
    const auto extracted = database.LoadMainAssetAtPath("Assets/Textures/ExtractedMask.ntex");
    ASSERT_TRUE(extracted.has_value());
    EXPECT_EQ(extracted->artifactType, ArtifactType::Texture);
    EXPECT_TRUE(database.Contains(*extracted));
    EXPECT_TRUE(database.IsNativeAsset(*extracted));
    EXPECT_FALSE(database.IsForeignAsset(*extracted));

    AssetDatabaseRecord foreign;
    foreign.assetId = AssetId::New();
    foreign.assetPath = "Packages/External/Foreign.mat";
    foreign.subAssetKey = "material:Foreign";
    foreign.artifactType = ArtifactType::Material;
    foreign.mainAsset = true;
    EXPECT_FALSE(database.Contains(foreign));
    EXPECT_TRUE(database.IsForeignAsset(foreign));

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, CreateTextAssetWritesSourceAndPreservesRequestedGuid)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto prefabId = ParseAssetId("d1010101-0101-4101-8101-010101010101");
    ASSERT_TRUE(database.CreateTextAsset(
        "{\n  \"format\": \"Nullus.ObjectGraph.Prefab\"\n}\n",
        "Assets/Prefabs/TextCreated.prefab",
        prefabId));

    EXPECT_TRUE(std::filesystem::exists(root / "Assets" / "Prefabs" / "TextCreated.prefab"));
    EXPECT_EQ(database.AssetPathToGUID("Assets/Prefabs/TextCreated.prefab"), prefabId.ToString());
    const auto loaded = AssetMeta::Load(root / "Assets" / "Prefabs" / "TextCreated.prefab.meta");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->assetType, AssetType::Prefab);
    EXPECT_EQ(loaded->importerId, "prefab");

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ImportPrefabSourceAssetBuildsLoadablePrefabArtifact)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    NLS::Engine::GameObject gameObject("Lamp", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        ParseAssetId("e5050505-0505-4505-8505-050505050505"),
        "Assets/Prefabs/Lamp.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.CreateTextAsset(
        created.prefabSourceText,
        "Assets/Prefabs/Lamp.prefab",
        ParseAssetId("e6060606-0606-4606-8606-060606060606")));

    ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/Lamp.prefab"));

    const auto allAssets = database.LoadAllAssetsAtPath("Assets/Prefabs/Lamp.prefab");
    ASSERT_EQ(allAssets.size(), 1u);
    EXPECT_TRUE(allAssets.front().mainAsset);
    EXPECT_EQ(allAssets.front().subAssetKey, "prefab:Lamp");
    EXPECT_EQ(allAssets.front().artifactType, ArtifactType::Prefab);

    auto prefab = database.LoadPrefabArtifactAtPath("Assets/Prefabs/Lamp.prefab", "prefab:Lamp");
    ASSERT_TRUE(prefab.has_value());
    EXPECT_FALSE(prefab->generatedModelPrefab);
    EXPECT_FALSE(prefab->Validate().HasErrors());
    EXPECT_EQ(prefab->graph.root.GetGuid().ToString(), created.artifact->graph.root.GetGuid().ToString());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, FileWatcherPreimportImportsSavedPrefabWithExternalAssetReferences)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Engine::Serialize;

    const auto root = MakeAssetDatabaseFacadeRoot();
    NLS::Engine::GameObject gameObject("RenderableCube", "Prefab");
    auto* meshFilter = gameObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = gameObject.AddComponent<NLS::Engine::Components::MeshRenderer>();

    const auto meshAssetId = ParseAssetId("e7070707-0707-4707-8707-070707070707");
    const auto materialAssetId = ParseAssetId("e8080808-0808-4808-8808-080808080808");
    const std::string meshArtifactPath = "Library/Artifacts/Cube/mesh.nmesh";
    const std::string materialArtifactPath = "Library/Artifacts/Cube/material.nmat";
    const auto meshReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(meshAssetId.GetGuid()),
        MakeLocalIdentifierInFile(meshAssetId.GetGuid(), meshArtifactPath),
        meshArtifactPath);
    const auto materialReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(materialAssetId.GetGuid()),
        MakeLocalIdentifierInFile(materialAssetId.GetGuid(), materialArtifactPath),
        materialArtifactPath);
    meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(meshReference));
    meshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(materialReference)
    });

    const auto prefabId = ParseAssetId("e9090909-0909-4909-8909-090909090909");
    const auto created = PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/RenderableCube.prefab"
    });
    ASSERT_EQ(created.status, PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(created.artifact.has_value());
    ASSERT_FALSE(created.artifact->Validate().HasErrors());

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.CreateTextAsset(
        created.prefabSourceText,
        "Assets/Prefabs/RenderableCube.prefab",
        prefabId));

    AssetPreimportScheduler scheduler;
    ImportProgressTracker tracker;
    EXPECT_TRUE(scheduler.Run(database, tracker, {
        AssetPreimportReason::FileWatcherChanged,
        {root / "Assets" / "Prefabs" / "RenderableCube.prefab"}
    }));

    auto prefab = database.LoadPrefabArtifactAtPath(
        "Assets/Prefabs/RenderableCube.prefab",
        "prefab:RenderableCube");
    ASSERT_TRUE(prefab.has_value());
    EXPECT_FALSE(prefab->Validate().HasErrors());

    std::filesystem::remove_all(root);
}
