#include <gtest/gtest.h>

#include <sstream>

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
#include "Assets/ExternalAssetImporter.h"
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
#include <cstdint>
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

std::string StableArtifactBlobFileName(
    const NLS::Core::Assets::AssetId owner,
    const std::string& subAssetKey)
{
    return NLS::Core::Assets::BuildArtifactStorageFileName(owner.ToString() + ":" + subAssetKey);
}

std::string FileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return {};

    const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
    return std::to_string(size) + ":" + std::to_string(writeTimeTicks);
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
        artifactPath = (std::filesystem::path("Library") /
            "Artifacts" /
            NLS::Core::Assets::BuildArtifactStorageRelativePath(StableArtifactBlobFileName(owner, subAssetKey))).generic_string();
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

void WriteManifestArtifactFiles(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    for (const auto& artifact : manifest.subAssets)
        WriteTextFile(root / artifact.artifactPath, artifact.subAssetKey);
}

void WritePersistedArtifactManifest(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    NLS::Core::Assets::ArtifactDatabase database;
    const auto databasePath = root / "Library" / "ArtifactDB";
    if (std::filesystem::exists(databasePath))
        (void)database.Load(databasePath);

    database.UpsertManifest(
        manifest,
        (std::filesystem::path("Assets") / manifest.sourceAssetId.ToString()).generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(databasePath));
}

void AddCurrentSourceDependencies(
    const std::filesystem::path& root,
    NLS::Core::Assets::ArtifactManifest& manifest,
    const std::string& assetPath)
{
    const auto hasTextureArtifact = std::any_of(
        manifest.subAssets.begin(),
        manifest.subAssets.end(),
        [](const NLS::Core::Assets::ImportedArtifact& artifact)
        {
            return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
        });
    const auto sourcePath = root / std::filesystem::path(assetPath);
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
        assetPath,
        FileStamp(sourcePath)
    });
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
        assetPath + ".meta",
        FileStamp(NLS::Core::Assets::GetAssetMetaPath(sourcePath))
    });
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::ImporterVersion,
        manifest.importerId,
        std::to_string(manifest.importerVersion)
    });
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::BuildTarget,
        manifest.targetPlatform,
        manifest.targetPlatform
    });
    if (hasTextureArtifact)
    {
        manifest.dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
            NLS::Editor::Assets::kExternalTextureBuildPipelineDependencyName,
            std::to_string(NLS::Editor::Assets::kExternalTexturePostprocessorVersion)
        });
    }
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

bool ContainsManifestDependency(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetDependencyKind kind,
    const std::string& value)
{
    return std::any_of(
        manifest.dependencies.begin(),
        manifest.dependencies.end(),
        [&](const NLS::Core::Assets::AssetDependencyRecord& dependency)
        {
            return dependency.kind == kind &&
                dependency.value == value &&
                !dependency.hashOrVersion.empty();
        });
}

std::optional<NLS::Core::Assets::ArtifactManifest> LoadPersistedArtifactManifest(
    const std::filesystem::path& root,
    const NLS::Core::Assets::AssetId sourceAssetId)
{
    NLS::Core::Assets::ArtifactDatabase database;
    if (!database.Load(root / "Library" / "ArtifactDB"))
        return std::nullopt;
    return database.BuildManifestForSource(sourceAssetId);
}

void RemovePersistedArtifactDependency(
    const std::filesystem::path& root,
    const NLS::Core::Assets::AssetId sourceAssetId,
    const NLS::Core::Assets::AssetDependencyKind kind,
    const std::string& value)
{
    const auto databasePath = root / "Library" / "ArtifactDB";
    NLS::Core::Assets::ArtifactDatabase database;
    ASSERT_TRUE(database.Load(databasePath));
    auto manifest = database.BuildManifestForSource(sourceAssetId);
    ASSERT_TRUE(manifest.has_value());
    manifest->dependencies.erase(
        std::remove_if(
            manifest->dependencies.begin(),
            manifest->dependencies.end(),
            [&](const NLS::Core::Assets::AssetDependencyRecord& dependency)
            {
                return dependency.kind == kind && dependency.value == value;
            }),
        manifest->dependencies.end());
    database.UpsertManifest(
        *manifest,
        (std::filesystem::path("Assets") / sourceAssetId.ToString()).generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(databasePath));
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

TEST(AssetDatabaseFacadeTests, ManifestQueriesExposeOnlyContentStorageArtifactPayloads)
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
    manifest.primarySubAssetKey = "mesh:Body";
    manifest.subAssets.push_back(MakeArtifact(
        modelId,
        "mesh:Body",
        ArtifactType::Mesh,
        "mesh",
        "Library/Artifacts/" + modelId.ToString() + "/meshes/not-a-content-addressed-blob"));
    manifest.subAssets.push_back(MakeArtifact(
        modelId,
        "material:Body",
        ArtifactType::Material,
        "material"));

    database.AddArtifactManifest(manifest);

    const auto allAssets = database.LoadAllAssetsAtPath("Assets/Models/Hero.gltf");
    ASSERT_EQ(allAssets.size(), 1u);
    EXPECT_EQ(allAssets[0].subAssetKey, "material:Body");
    EXPECT_EQ(allAssets[0].artifactType, ArtifactType::Material);
    EXPECT_FALSE(database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "mesh:Body").has_value());
    EXPECT_TRUE(database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "material:Body").has_value());

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_EQ(artifactDatabase.Find(modelId, "mesh:Body", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(modelId, "material:Body", "editor"), nullptr);

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

    const auto prefabRecord = database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "prefab:Hero");
    ASSERT_TRUE(prefabRecord.has_value());
    EXPECT_TRUE(std::filesystem::exists(prefabRecord->artifactPath));
    EXPECT_FALSE(std::filesystem::path(prefabRecord->artifactPath).filename().has_extension());
    const auto meshRecordAsset = database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "mesh:mesh/0");
    ASSERT_TRUE(meshRecordAsset.has_value());
    const auto meshArtifactPath = std::filesystem::path(meshRecordAsset->artifactPath);
    EXPECT_TRUE(std::filesystem::exists(meshArtifactPath));
    EXPECT_FALSE(meshArtifactPath.filename().has_extension());
    const auto materialRecordAsset = database.LoadSubAssetAtPath("Assets/Models/Hero.gltf", "material:material/0");
    ASSERT_TRUE(materialRecordAsset.has_value());
    EXPECT_TRUE(std::filesystem::exists(materialRecordAsset->artifactPath));
    EXPECT_FALSE(std::filesystem::path(materialRecordAsset->artifactPath).filename().has_extension());

    const auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshArtifactPath);
    ASSERT_TRUE(meshArtifact.has_value());
    EXPECT_EQ(meshArtifact->vertices.size(), 3u);
    EXPECT_EQ(meshArtifact->indices.size(), 3u);
    EXPECT_EQ(meshArtifact->materialIndex, 0u);
    EXPECT_FLOAT_EQ(meshArtifact->vertices[1].position[0], 1.0f);
    EXPECT_FLOAT_EQ(meshArtifact->vertices[2].position[1], 1.0f);

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    const auto* meshRecord = artifactDatabase.Find(sourceId, "mesh:mesh/0", "editor");
    ASSERT_NE(meshRecord, nullptr);
    EXPECT_EQ(meshRecord->sourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(meshRecord->artifactPath, std::filesystem::path(meshRecordAsset->artifactPath).lexically_relative(root).generic_string());
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
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
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
        EXPECT_TRUE(IsContentStorageArtifactPath(record->artifactPath)) << record->artifactPath;
        EXPECT_EQ(record->artifactPath.find("Library/Artifacts/"), 0u) << record->artifactPath;
        const auto blobName = std::filesystem::path(record->artifactPath).filename().generic_string();
        EXPECT_TRUE(IsArtifactStorageFileName(blobName)) << record->artifactPath;
        EXPECT_EQ(std::filesystem::path(record->artifactPath).parent_path().filename().generic_string(), blobName.substr(0u, 2u))
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

TEST(AssetDatabaseFacadeTests, ImportedModelMaterialReferencesAuthoritativeShaderLabSource)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader",
        R"(
Shader "Nullus/StandardPBR"
{
    Properties
    {
        _BaseColor("Base Color", Color) = (1, 1, 1, 1)
    }

    SubShader
    {
        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
}
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
    ASSERT_TRUE(database.ImportAsset("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"));
    ASSERT_TRUE(database.ImportAsset("Assets/Models/Hero.gltf"));

    const auto modelId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    const auto modelManifest = database.GetArtifactManifestForAssetPath("Assets/Models/Hero.gltf");
    ASSERT_TRUE(modelManifest.has_value());
    const auto* materialArtifact = modelManifest->FindSubAsset("material:material/0");
    ASSERT_NE(materialArtifact, nullptr);

    const auto materialPayload = ReadArtifactPayloadText(
        root / materialArtifact->artifactPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    const auto shaderId = database.AssetPathToGUID("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader");
    ASSERT_FALSE(shaderId.empty());
    const auto shaderManifest = database.GetArtifactManifestForAssetPath("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader");
    ASSERT_TRUE(shaderManifest.has_value());
    const auto* shaderArtifact = shaderManifest->FindPrimaryArtifact();
    ASSERT_NE(shaderArtifact, nullptr);
    const auto shaderResourcePath = shaderArtifact->artifactPath;
    EXPECT_EQ(shaderResourcePath.find("Library/Artifacts/"), 0u);
    EXPECT_FALSE(std::filesystem::path(shaderResourcePath).is_absolute());
    EXPECT_NE(
        materialPayload.find("shader=Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"),
        std::string::npos);
    EXPECT_EQ(materialPayload.find("shader=" + shaderResourcePath), std::string::npos);
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
        root / "Assets" / "Shaders" / "HeroSurface.shader",
        R"(Shader "Tests/HeroSurface"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/HeroSurface.shader"));

    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Shaders/HeroSurface.shader"));
    ASSERT_TRUE(sourceId.IsValid());

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/HeroSurface.shader");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->subAssetKey, "shader:HeroSurface");
    EXPECT_EQ(mainAsset->artifactType, ArtifactType::Shader);
    EXPECT_TRUE(mainAsset->mainAsset);
    EXPECT_TRUE(std::filesystem::exists(mainAsset->artifactPath));
    EXPECT_FALSE(std::filesystem::path(mainAsset->artifactPath).filename().has_extension());

    const auto artifactPayload = ReadTextFile(mainAsset->artifactPath);
    ASSERT_FALSE(artifactPayload.empty());
    EXPECT_NE(artifactPayload.find("NULLUS_IMPORTED_SHADER_ARTIFACT=1"), std::string::npos);
    EXPECT_NE(artifactPayload.find("SOURCE=Assets/Shaders/HeroSurface.shader"), std::string::npos);
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

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/HeroSurface.shader");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ(manifest->sourceAssetId, sourceId);
    EXPECT_EQ(manifest->importerId, "shader");
    EXPECT_EQ(manifest->primarySubAssetKey, "shader:HeroSurface");
    ASSERT_NE(manifest->FindSubAsset("shader:HeroSurface"), nullptr);
    EXPECT_EQ(manifest->FindSubAsset("shader:HeroSurface")->artifactPath.find("Library/Artifacts/"), 0u);
    EXPECT_FALSE(std::filesystem::path(manifest->FindSubAsset("shader:HeroSurface")->artifactPath).is_absolute());
    EXPECT_TRUE(std::any_of(
        manifest->dependencies.begin(),
        manifest->dependencies.end(),
        [](const AssetDependencyRecord& dependency)
        {
            return dependency.kind == AssetDependencyKind::BuildTarget &&
                dependency.value == "editor";
        }));
    EXPECT_TRUE(std::any_of(
        manifest->dependencies.begin(),
        manifest->dependencies.end(),
        [](const AssetDependencyRecord& dependency)
        {
            return dependency.kind == AssetDependencyKind::PostprocessorVersion &&
                dependency.value == "shader-compiler-toolchain" &&
                !dependency.hashOrVersion.empty();
        }));

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    const auto* record = artifactDatabase.Find(sourceId, "shader:HeroSurface", "editor");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->sourcePath, "Assets/Shaders/HeroSurface.shader");
    EXPECT_EQ(record->artifactType, ArtifactType::Shader);
    EXPECT_EQ(record->loaderId, "shader");
    EXPECT_EQ(record->artifactPath, std::filesystem::path(mainAsset->artifactPath).lexically_relative(root).generic_string());

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetDatabaseFacadeTests, ShaderArtifactManifestCurrentRejectsMissingCompilerToolchainDependency)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "Shader artifact import success currently requires Windows DXC process execution.";
#else
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "ToolchainFreshness.shader",
        R"(Shader "Tests/ToolchainFreshness"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade importer(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(importer.Refresh());
    ASSERT_TRUE(importer.ImportAsset("Assets/Shaders/ToolchainFreshness.shader"));
    ASSERT_TRUE(importer.IsArtifactManifestCurrentForAssetPath("Assets/Shaders/ToolchainFreshness.shader"));

    RemovePersistedArtifactDependency(
        root,
        ParseAssetId(importer.AssetPathToGUID("Assets/Shaders/ToolchainFreshness.shader")),
        NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
        "shader-compiler-toolchain");

    AssetDatabaseFacade restarted(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(restarted.Refresh());
    EXPECT_FALSE(restarted.IsArtifactManifestCurrentForAssetPath("Assets/Shaders/ToolchainFreshness.shader"));

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportWritesMultiCompileVariantsButNotMaterialFeatures)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "ShaderLab import success currently requires Windows DXC process execution.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "KeywordCombo.shader",
        R"(Shader "Tests/KeywordCombo"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            #pragma shader_feature _ALPHATEST_ON
            #pragma multi_compile _ MAIN_LIGHT_SHADOWS
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target
            {
                float4 color = float4(1, 1, 1, 1);
            #if defined(_ALPHATEST_ON)
                color.r = 0.5;
            #endif
            #if defined(MAIN_LIGHT_SHADOWS)
                color.g = 0.25;
            #endif
                return color;
            }
            ENDHLSL
        }
    }
}
)");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/KeywordCombo.shader"));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/KeywordCombo.shader");
    ASSERT_TRUE(manifest.has_value());
    const auto* shaderArtifact = manifest->FindPrimaryArtifact();
    ASSERT_NE(shaderArtifact, nullptr);
    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(root / shaderArtifact->artifactPath);
    ASSERT_TRUE(artifact.has_value());

    NLS::Render::ShaderLab::ShaderLabKeywordSet combination;
    combination.Enable("MAIN_LIGHT_SHADOWS");
    const auto multiCompileHash = combination.Hash();
    NLS::Render::ShaderLab::ShaderLabKeywordSet materialFeature;
    materialFeature.Enable("_ALPHATEST_ON");
    const auto materialFeatureHash = materialFeature.Hash();

    const auto hasMultiCompilePixelStage = std::any_of(
        artifact->stages.begin(),
        artifact->stages.end(),
        [multiCompileHash](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.stage == NLS::Render::ShaderCompiler::ShaderStage::Pixel &&
                stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL &&
                stage.keywordHash == multiCompileHash &&
                stage.output.status == NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
        });
    EXPECT_TRUE(hasMultiCompilePixelStage);

    const auto hasMaterialFeaturePixelStage = std::any_of(
        artifact->stages.begin(),
        artifact->stages.end(),
        [materialFeatureHash](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.stage == NLS::Render::ShaderCompiler::ShaderStage::Pixel &&
                stage.keywordHash == materialFeatureHash;
        });
    EXPECT_FALSE(hasMaterialFeaturePixelStage);

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportReflectionUsesDefaultVariantOnly)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "ShaderLab import success currently requires Windows DXC process execution.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "KeywordResource.shader",
        R"(Shader "Tests/KeywordResource"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            #pragma multi_compile _ MAIN_LIGHT_SHADOWS

            struct Attributes { float3 positionOS : POSITION; };
            struct Varyings { float4 positionCS : SV_POSITION; };

            Texture2D _BaseMap : register(t0, space2);
            SamplerState sampler_BaseMap : register(s0, space2);

            #if defined(MAIN_LIGHT_SHADOWS)
            Texture2D _ShadowMap : register(t1, space2);
            SamplerState sampler_ShadowMap : register(s1, space2);
            #endif

            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionCS = float4(input.positionOS, 1.0);
                return output;
            }

            float4 PSMain(Varyings input) : SV_Target
            {
                float4 color = _BaseMap.Sample(sampler_BaseMap, float2(0.0, 0.0));
            #if defined(MAIN_LIGHT_SHADOWS)
                color *= _ShadowMap.Sample(sampler_ShadowMap, float2(0.0, 0.0));
            #endif
                return color;
            }
            ENDHLSL
        }
    }
}
)");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/KeywordResource.shader"));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/KeywordResource.shader");
    ASSERT_TRUE(manifest.has_value());
    const auto* shaderArtifact = manifest->FindPrimaryArtifact();
    ASSERT_NE(shaderArtifact, nullptr);
    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(root / shaderArtifact->artifactPath);
    ASSERT_TRUE(artifact.has_value());

    const auto hasTexture =
        [&artifact](const std::string& name)
    {
        return std::any_of(
            artifact->reflection.properties.begin(),
            artifact->reflection.properties.end(),
            [&name](const NLS::Render::Resources::ShaderPropertyDesc& property)
            {
                return property.name == name;
            });
    };

    EXPECT_TRUE(hasTexture("_BaseMap"));
    EXPECT_FALSE(hasTexture("_ShadowMap"));

    NLS::Render::ShaderLab::ShaderLabKeywordSet shadows;
    shadows.Enable("MAIN_LIGHT_SHADOWS");
    const auto shadowKeywordHash = shadows.Hash();
    EXPECT_TRUE(std::any_of(
        artifact->stages.begin(),
        artifact->stages.end(),
        [shadowKeywordHash](const NLS::Render::Assets::ShaderArtifactStage& stage)
        {
            return stage.stage == NLS::Render::ShaderCompiler::ShaderStage::Pixel &&
                stage.targetPlatform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL &&
                stage.keywordHash == shadowKeywordHash &&
                stage.output.status == NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded;
        }));

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportWritesLightModePassSubAssets)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "ShaderLab artifact import success currently requires Windows DXC process execution.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "MultiPass.shader",
        R"(Shader "Tests/MultiPass"
{
    SubShader
    {
        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
        Pass
        {
            Name "DepthOnly"
            Tags { "LightMode" = "DepthOnly" }
            ZWrite On
            ZTest LessEqual
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/MultiPass.shader"));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/MultiPass.shader");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ(manifest->primarySubAssetKey, "shader:MultiPass");
    ASSERT_NE(manifest->FindSubAsset("shader:MultiPass"), nullptr);
    const auto* depthOnly = manifest->FindSubAsset("shader:MultiPass/DepthOnly#1");
    ASSERT_NE(depthOnly, nullptr);
    EXPECT_EQ(depthOnly->artifactType, NLS::Core::Assets::ArtifactType::Shader);
    EXPECT_EQ(depthOnly->artifactPath.find("Library/Artifacts/"), 0u);

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportDisambiguatesDuplicateLightModePassSubAssets)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "ShaderLab artifact import success currently requires Windows DXC process execution.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "DuplicateForward.shader",
        R"(Shader "Tests/DuplicateForward"
{
    SubShader
    {
        Pass
        {
            Name "ForwardOpaque"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 0, 0, 1); }
            ENDHLSL
        }
        Pass
        {
            Name "ForwardAlphaTest"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(0, 1, 0, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/DuplicateForward.shader"));

    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Shaders/DuplicateForward.shader");
    ASSERT_TRUE(manifest.has_value());
    EXPECT_NE(manifest->FindSubAsset("shader:DuplicateForward"), nullptr);
    EXPECT_NE(manifest->FindSubAsset("shader:DuplicateForward/ForwardAlphaTest#1"), nullptr);
    EXPECT_EQ(manifest->subAssets.size(), 2u);

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportGeneratedSourcePathIncludesAssetPathToAvoidSameStemCollisions)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "ShaderLab artifact import success currently requires Windows DXC process execution.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto shaderSource = [](const char* name, const char* red)
    {
        std::ostringstream stream;
        stream << "Shader \"Tests/" << name << R"("
{
    SubShader
    {
        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4()" << red << R"(, 0, 0, 1); }
            ENDHLSL
        }
    }
})";
        return stream.str();
    };

    WriteTextFile(root / "Assets" / "Shaders" / "A" / "Foo.shader", shaderSource("A/Foo", "0.25"));
    WriteTextFile(root / "Assets" / "Shaders" / "B" / "Foo.shader", shaderSource("B/Foo", "0.75"));

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/A/Foo.shader"));
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/B/Foo.shader"));

    const auto shaderCacheRoot = root / "Library" / "ShaderCache" / "ImportedShaderLab";
    std::vector<std::filesystem::path> generatedFooSources;
    for (const auto& entry : std::filesystem::directory_iterator(shaderCacheRoot))
    {
        if (!entry.is_regular_file())
            continue;
        const auto fileName = entry.path().filename().generic_string();
        if (fileName.rfind("Foo_", 0u) == 0u && entry.path().extension() == ".hlsl")
            generatedFooSources.push_back(entry.path());
    }

    ASSERT_GE(generatedFooSources.size(), 2u);
    std::unordered_set<std::string> generatedNames;
    bool foundA = false;
    bool foundB = false;
    for (const auto& generatedPath : generatedFooSources)
    {
        generatedNames.insert(generatedPath.filename().generic_string());
        const auto generatedText = ReadTextFile(generatedPath);
        foundA = foundA || generatedText.find("Assets/Shaders/A/Foo.shader") != std::string::npos;
        foundB = foundB || generatedText.find("Assets/Shaders/B/Foo.shader") != std::string::npos;
    }

    EXPECT_EQ(generatedNames.size(), generatedFooSources.size());
    EXPECT_TRUE(foundA);
    EXPECT_TRUE(foundB);

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
        root / "Assets" / "Shaders" / "Warmup.shader",
        R"(Shader "Tests/Warmup"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());

    AssetPreimportScheduler scheduler;
    auto coldPlan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_NE(
        std::find(coldPlan.assetPaths.begin(), coldPlan.assetPaths.end(), "Assets/Shaders/Warmup.shader"),
        coldPlan.assetPaths.end());

    ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, AssetPreimportReason::EditorStartup));
    auto warmPlan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_EQ(
        std::find(warmPlan.assetPaths.begin(), warmPlan.assetPaths.end(), "Assets/Shaders/Warmup.shader"),
        warmPlan.assetPaths.end());

    WriteTextFile(
        root / "Assets" / "Shaders" / "Warmup.shader",
        R"(Shader "Tests/Warmup"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(1, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(0, 1, 0, 1); }
            ENDHLSL
        }
    }
})");
    ASSERT_TRUE(database.Refresh());
    auto changedPlan = scheduler.BuildPlan(database, {AssetPreimportReason::FileWatcherChanged, {root / "Assets" / "Shaders" / "Warmup.shader"}});
    EXPECT_NE(
        std::find(changedPlan.assetPaths.begin(), changedPlan.assetPaths.end(), "Assets/Shaders/Warmup.shader"),
        changedPlan.assetPaths.end());

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetDatabaseFacadeTests, StartupPreimportPlanReimportsShaderArtifactsWithoutUsableStages)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Shaders" / "Broken.shader", R"(Shader "Tests/Broken"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            float4 NotAnEntry() : SV_Target { return 0; }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database.ImportAsset("Assets/Shaders/Broken.shader"));
    const auto sourceRecord = database.LoadMainAssetAtPath("Assets/Shaders/Broken.shader");
    ASSERT_TRUE(sourceRecord.has_value());
    EXPECT_TRUE(sourceRecord->artifactPath.empty());
    EXPECT_FALSE(database.GetArtifactManifestForAssetPath("Assets/Shaders/Broken.shader").has_value());

    AssetPreimportScheduler scheduler;
    auto plan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    EXPECT_NE(
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Shaders/Broken.shader"),
        plan.assetPaths.end());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderLabParseFailureDoesNotPublishFailedArtifactOrFallbackCompileWholeShaderSource)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Shaders" / "Malformed.shader", R"(Shader "Tests/Malformed"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            struct Attributes { float3 positionOS : POSITION; };
            struct Varyings { float4 positionCS : SV_POSITION; };
            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionCS = float4(input.positionOS, 1);
                return output;
            }
            float4 PSMain(Varyings input) : SV_Target0 { return 1.xxxx; }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/Malformed.shader"));

    const auto previousMainAsset = database.LoadMainAssetAtPath("Assets/Shaders/Malformed.shader");
    ASSERT_TRUE(previousMainAsset.has_value());
    const auto previousArtifactPath = previousMainAsset->artifactPath;

    WriteTextFile(root / "Assets" / "Shaders" / "Malformed.shader", R"(Shader "Tests/Malformed"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            float4 PSMain() : SV_Target0 { return 1.xxxx; }
        }
    }
})");

    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database.ImportAsset("Assets/Shaders/Malformed.shader"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/Malformed.shader");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->artifactPath, previousArtifactPath);
    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(mainAsset->artifactPath);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_TRUE(NLS::Render::Assets::HasUsableShaderArtifactStage(*artifact));
    const auto diagnostics = database.GetDiagnostics();
    const auto foundMissingEndHlsl = std::find_if(
        diagnostics.begin(),
        diagnostics.end(),
        [](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.message.find("missing ENDHLSL") != std::string::npos;
        });
    EXPECT_NE(foundMissingEndHlsl, diagnostics.end());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, ShaderLabImportUsesFirstLightModePassWhenForwardIsMissing)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Shaders" / "DepthOnly.shader", R"(Shader "Tests/DepthOnly"
{
    SubShader
    {
        Pass
        {
            Name "DepthOnly"
            Tags { "LightMode" = "DepthOnly" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/DepthOnly.shader"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/DepthOnly.shader");
    ASSERT_TRUE(mainAsset.has_value());
    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(mainAsset->artifactPath);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_TRUE(NLS::Render::Assets::HasUsableShaderArtifactStage(*artifact));
    ASSERT_TRUE(artifact->shaderLabPassState.has_value());
    EXPECT_TRUE(artifact->shaderLabPassState->depthWrite);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, StartupPreimportPlanReimportsShaderArtifactsMissingGlslStages)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(
        root / "Assets" / "Shaders" / "LegacyWarm.shader",
        R"(Shader "Tests/LegacyWarm"
{
    SubShader
    {
        Pass
        {
            Tags { "LightMode" = "Forward" }
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 VSMain() : SV_Position { return float4(0, 0, 0, 1); }
            float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
            ENDHLSL
        }
    }
})");

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Shaders/LegacyWarm.shader"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Shaders/LegacyWarm.shader");
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
        std::find(plan.assetPaths.begin(), plan.assetPaths.end(), "Assets/Shaders/LegacyWarm.shader"),
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
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
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
    WriteTextFile(root / "Library" / "ArtifactDB", "corrupted central index\n");
    database.AddArtifactManifest(heroB);

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
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
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "ArtifactDB"));
    EXPECT_TRUE(database.StopAssetEditing());

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    EXPECT_NE(artifactDatabase.Find(heroAId, "model:HeroA", "editor"), nullptr);
    EXPECT_NE(artifactDatabase.Find(heroBId, "model:HeroB", "editor"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, StopAssetEditingReportsArtifactDatabaseSaveFailure)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Models" / "Hero.gltf", R"({"asset":{"version":"2.0"}})");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto heroId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = heroId;
    manifest.importerId = "scene-model";
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "model:Hero";
    manifest.subAssets.push_back(MakeArtifact(heroId, "model:Hero", ArtifactType::Model, "model"));

    database.StartAssetEditing();
    database.AddArtifactManifest(manifest);

    const auto databasePath = root / "Library" / "ArtifactDB";
    std::filesystem::create_directories(databasePath.parent_path());
    WriteTextFile(databasePath, "blocked by file\n");

    EXPECT_FALSE(database.StopAssetEditing());
    EXPECT_TRUE(std::filesystem::is_regular_file(databasePath));
    ASSERT_FALSE(database.GetDiagnostics().empty());
    EXPECT_NE(database.GetDiagnostics().back().message.find("ArtifactDB could not be saved"), std::string::npos);
    EXPECT_NE(database.GetDiagnostics().back().message.find("not a directory"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RefreshDoesNotWarnWhenCentralArtifactDatabaseIsMissing)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Fresh.prefab", "Prefab \"Fresh\" {}\n");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    const auto hasArtifactDbReadFailure = std::any_of(
        database.GetDiagnostics().begin(),
        database.GetDiagnostics().end(),
        [](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.code == "assetdatabase-artifactdb-read-failed";
        });
    EXPECT_FALSE(hasArtifactDbReadFailure)
        << "A missing ArtifactDB is normal for a fresh Library and should not be reported as corruption.";

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
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "ArtifactDB"));

    ASSERT_TRUE(database.Refresh());

    ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
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
        {root / "Assets", false, "Assets", root / "Library"},
        {packageRoot, false, "Packages", root / "Library"}
    });
    ASSERT_TRUE(importer.Refresh());
    ASSERT_TRUE(importer.ImportAsset("Packages/Models/LegacyReadOnlyHero.gltf"));
    {
        const auto sourceId = ParseAssetId(importer.AssetPathToGUID("Packages/Models/LegacyReadOnlyHero.gltf"));
        const auto databasePath = root / "Library" / "ArtifactDB";
        ArtifactDatabase artifactDatabase;
        ASSERT_TRUE(artifactDatabase.Load(databasePath));
        auto manifest = artifactDatabase.BuildManifestForSource(sourceId);
        ASSERT_TRUE(manifest.has_value());
        manifest->importerVersion = 5u;
        artifactDatabase.UpsertManifest(
            *manifest,
            "Packages/Models/LegacyReadOnlyHero.gltf",
            ArtifactRecordStatus::UpToDate);
        ASSERT_TRUE(artifactDatabase.Save(databasePath));
    }

    AssetDatabaseFacade readOnlyDatabase({
        {root / "Assets", false, "Assets", root / "Library"},
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

    RemovePersistedArtifactDependency(
        root,
        ParseAssetId(database.AssetPathToGUID("Assets/Models/TexturePipelineHero.obj")),
        NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
        "external-texture-build-pipeline");

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

    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.gltf"));
    const auto manifest = LoadPersistedArtifactManifest(root, sourceId);
    ASSERT_TRUE(manifest.has_value());
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/Hero.gltf"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/Hero.bin"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Textures/HeroBaseColor.png"));

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

    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Hero.obj"));
    const auto manifest = LoadPersistedArtifactManifest(root, sourceId);
    ASSERT_TRUE(manifest.has_value());
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/Hero.obj"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/Hero.mtl"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/HeroExtra.mtl"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Textures/HeroDiffuse.png"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Textures/HeroNormal.png"));

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

    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/jeep1.fbx"));
    const auto manifest = LoadPersistedArtifactManifest(root, sourceId);
    ASSERT_TRUE(manifest.has_value());
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/jeep1.fbx"));
    EXPECT_TRUE(ContainsManifestDependency(*manifest, NLS::Core::Assets::AssetDependencyKind::SourceFileHash, "Assets/Models/jeep1.jpg"));

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

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    if (artifactDatabase.Load(root / "Library" / "ArtifactDB"))
    {
        const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Broken.fbx"));
        EXPECT_FALSE(artifactDatabase.BuildManifestForSource(sourceId).has_value());
    }
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

    const auto firstPrimitiveRecord = database.LoadSubAssetAtPath(
        "Assets/Models/TwoTriangles.gltf",
        "mesh:mesh/0/primitive/0");
    const auto secondPrimitiveRecord = database.LoadSubAssetAtPath(
        "Assets/Models/TwoTriangles.gltf",
        "mesh:mesh/0/primitive/1");
    ASSERT_TRUE(firstPrimitiveRecord.has_value());
    ASSERT_TRUE(secondPrimitiveRecord.has_value());
    const auto firstPrimitiveArtifact = NLS::Render::Assets::LoadMeshArtifact(
        firstPrimitiveRecord->artifactPath);
    const auto secondPrimitiveArtifact = NLS::Render::Assets::LoadMeshArtifact(
        secondPrimitiveRecord->artifactPath);

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
    auto meshRecord = database.LoadSubAssetAtPath("Assets/Models/Reimported.gltf", "mesh:mesh/0");
    ASSERT_TRUE(meshRecord.has_value());
    const auto meshPath = std::filesystem::path(meshRecord->artifactPath);
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
    const auto firstPrimitiveRecord = database.LoadSubAssetAtPath(
        "Assets/Models/Reimported.gltf",
        "mesh:mesh/0/primitive/0");
    const auto secondPrimitiveRecord = database.LoadSubAssetAtPath(
        "Assets/Models/Reimported.gltf",
        "mesh:mesh/0/primitive/1");
    ASSERT_TRUE(firstPrimitiveRecord.has_value());
    ASSERT_TRUE(secondPrimitiveRecord.has_value());
    const auto firstPrimitivePath = std::filesystem::path(firstPrimitiveRecord->artifactPath);
    const auto secondPrimitivePath = std::filesystem::path(secondPrimitiveRecord->artifactPath);
    EXPECT_FALSE(database.LoadSubAssetAtPath("Assets/Models/Reimported.gltf", "mesh:mesh/0").has_value());
    const auto reimportedManifest = database.GetArtifactManifestForAssetPath("Assets/Models/Reimported.gltf");
    ASSERT_TRUE(reimportedManifest.has_value());
    EXPECT_EQ(reimportedManifest->FindSubAsset("mesh:mesh/0"), nullptr);
    EXPECT_NE(reimportedManifest->FindSubAsset("mesh:mesh/0/primitive/0"), nullptr);
    EXPECT_NE(reimportedManifest->FindSubAsset("mesh:mesh/0/primitive/1"), nullptr);

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
    ASSERT_TRUE(artifactDatabase.Load(root / "Library" / "ArtifactDB"));
    const auto sourceId = ParseAssetId(database.AssetPathToGUID("Assets/Models/Textured.gltf"));
    const auto* textureRecord = artifactDatabase.Find(sourceId, "texture:image/0", "win64-dx12");
    if (textureRecord == nullptr)
        textureRecord = artifactDatabase.Find(sourceId, "texture:image/0", "editor");
    ASSERT_NE(textureRecord, nullptr);
    EXPECT_EQ(textureRecord->artifactType, ArtifactType::Texture);
    EXPECT_EQ(textureRecord->loaderId, "texture");
    EXPECT_FALSE(std::filesystem::path(textureRecord->artifactPath).is_absolute());
    EXPECT_TRUE(IsContentStorageArtifactPath(textureRecord->artifactPath)) << textureRecord->artifactPath;
    EXPECT_EQ(textureRecord->artifactPath.find("Library/Artifacts/"), 0u) << textureRecord->artifactPath;

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
    const auto meshRecord = database.LoadSubAssetAtPath("Assets/Models/Stable.gltf", "mesh:mesh/0");
    ASSERT_TRUE(meshRecord.has_value());
    const auto meshPath = std::filesystem::path(meshRecord->artifactPath);
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
    const auto meshRecord = database.LoadSubAssetAtPath("Assets/Models/Transactional.gltf", "mesh:mesh/0");
    ASSERT_TRUE(meshRecord.has_value());
    const auto meshPath = std::filesystem::path(meshRecord->artifactPath);
    const auto databasePath = root / "Library" / "ArtifactDB";
    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(meshPath);
    ASSERT_TRUE(meshArtifact.has_value());
    ASSERT_EQ(meshArtifact->vertices.size(), 3u);
    const auto originalMeshBytes = std::filesystem::file_size(meshPath);

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

    std::filesystem::remove_all(databasePath);
    WriteTextFile(databasePath, "not an lmdb environment\n");

    EXPECT_FALSE(database.ReimportAsset("Assets/Models/Transactional.gltf"));
    EXPECT_TRUE(std::filesystem::is_regular_file(databasePath));
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

    const auto prefabRecord = database.LoadSubAssetAtPath("Assets/Prefabs/Transactional.prefab", "prefab:Transactional");
    ASSERT_TRUE(prefabRecord.has_value());
    const auto prefabPayloadPath = std::filesystem::path(prefabRecord->artifactPath);
    const auto databasePath = root / "Library" / "ArtifactDB";
    ASSERT_TRUE(std::filesystem::exists(prefabPayloadPath));
    ASSERT_TRUE(std::filesystem::exists(databasePath));
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
    std::filesystem::remove_all(databasePath);
    WriteTextFile(databasePath, "not an lmdb environment\n");

    EXPECT_FALSE(database.ReimportAsset("Assets/Prefabs/Transactional.prefab"));
    EXPECT_TRUE(std::filesystem::is_regular_file(databasePath));
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

    const auto databasePath = root / "Library" / "ArtifactDB";
    std::filesystem::remove_all(databasePath);
    std::filesystem::create_directories(databasePath);

    ASSERT_TRUE(database.Refresh());
    EXPECT_FALSE(database
        .LoadPrefabArtifactAtPath("Assets/Prefabs/BrokenManifest.prefab", "prefab:BrokenManifest")
        .has_value());
    ASSERT_FALSE(database.GetDiagnostics().empty());
    EXPECT_NE(database.GetDiagnostics().back().message.find("ArtifactDB could not be read"), std::string::npos);
    EXPECT_NE(database.GetDiagnostics().back().message.find("mdb_env_open"), std::string::npos);

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
    EXPECT_EQ(gltfRoot, root / "Library" / "Artifacts");
    EXPECT_EQ(fbxRoot, root / "Library" / "Artifacts");
    EXPECT_EQ(gltfRoot, fbxRoot);
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
    manifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = "model:Hero";
    auto makeSafeArtifact = [modelId](
        const std::string& subAssetKey,
        const ArtifactType artifactType,
        const std::string& loaderId)
    {
        return MakeArtifact(modelId, subAssetKey, artifactType, loaderId);
    };
    manifest.subAssets.push_back(makeSafeArtifact("model:Hero", ArtifactType::Model, "model"));
    manifest.subAssets.push_back(makeSafeArtifact("prefab:Hero", ArtifactType::Prefab, "prefab"));
    manifest.subAssets.push_back(makeSafeArtifact("mesh:Body", ArtifactType::Mesh, "mesh"));
    manifest.subAssets.push_back(makeSafeArtifact("material:Body", ArtifactType::Material, "material"));
    manifest.subAssets.push_back(makeSafeArtifact("texture:Albedo", ArtifactType::Texture, "texture"));
    manifest.subAssets.push_back(makeSafeArtifact("shader:HeroSurface", ArtifactType::Shader, "shader"));
    manifest.subAssets.push_back(makeSafeArtifact("animation:Idle", ArtifactType::AnimationClip, "animation"));
    WriteManifestArtifactFiles(root, manifest);
    AddCurrentSourceDependencies(root, manifest, "Assets/Models/Hero.gltf");
    database.AddArtifactManifest(manifest);

    ASSERT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/Hero.gltf"));
    ASSERT_EQ(database.LoadAllAssetsAtPath("Assets/Models/Hero.gltf").size(), 7u);
    const auto entries = BuildAssetBrowserSubAssetEntries(database, "Assets/Models/Hero.gltf");
    ASSERT_EQ(entries.size(), 4u);

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

    EXPECT_EQ(entries[3].displayName, "HeroSurface");
    EXPECT_EQ(entries[3].subAssetKey, "shader:HeroSurface");
    EXPECT_EQ(entries[3].artifactType, ArtifactType::Shader);

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
    const auto prefabRecord = database.LoadSubAssetAtPath("Assets/Models/BridgeHero.gltf", "prefab:BridgeHero");
    ASSERT_TRUE(prefabRecord.has_value());
    EXPECT_TRUE(std::filesystem::exists(prefabRecord->artifactPath));
    EXPECT_FALSE(std::filesystem::path(prefabRecord->artifactPath).filename().has_extension());
    EXPECT_TRUE(std::filesystem::exists(root / "Library" / "ArtifactDB"));

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
        "Artifacts/11/1111111111111111111111111111111111111111111111111111111111111111",
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
        "Artifacts/22/2222222222222222222222222222222222222222222222222222222222222222",
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
        "Artifacts/33/3333333333333333333333333333333333333333333333333333333333333333",
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
        "Artifacts/44/4444444444444444444444444444444444444444444444444444444444444444",
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
    EXPECT_EQ(prefabEntry->artifactPath, "Artifacts/11/1111111111111111111111111111111111111111111111111111111111111111");
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
        (std::filesystem::path("Artifacts") /
            NLS::Core::Assets::BuildArtifactStorageRelativePath(
                "1111111111111111111111111111111111111111111111111111111111111111")).generic_string(),
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
    material.serializedPayload =
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/Unlit.shader\n"
        "property _BaseColor Color 1,1,1,1\n";

    ASSERT_TRUE(database.CreateAsset(material, "Assets/Materials/Hero.mat"));
    const auto materialSourcePath = root / "Assets" / "Materials" / "Hero.mat";
    ASSERT_TRUE(std::filesystem::exists(materialSourcePath));
    const auto materialSourceBytes = ReadBinaryFile(materialSourcePath);
    const auto materialSourceContainer = ReadNativeArtifactContainer(
        materialSourceBytes,
        ArtifactType::Material,
        1u);
    ASSERT_TRUE(materialSourceContainer.has_value());
    EXPECT_EQ(materialSourceContainer->metadata.schemaName, "material");
    EXPECT_EQ(materialSourceContainer->metadata.subAssetKey, "material:HeroMaterial");
    EXPECT_EQ(materialSourceContainer->metadata.displayName, "HeroMaterial");
    EXPECT_EQ(
        std::string(materialSourceContainer->payload.begin(), materialSourceContainer->payload.end()),
        material.serializedPayload);
    const auto materialGuid = database.AssetPathToGUID("Assets/Materials/Hero.mat");
    ASSERT_FALSE(materialGuid.empty());
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/Materials/Hero.mat"));

    const auto mainAsset = database.LoadMainAssetAtPath("Assets/Materials/Hero.mat");
    ASSERT_TRUE(mainAsset.has_value());
    EXPECT_EQ(mainAsset->artifactType, ArtifactType::Material);
    EXPECT_TRUE(database.Contains(*mainAsset));
    EXPECT_TRUE(database.IsMainAsset(*mainAsset));
    EXPECT_FALSE(database.IsSubAsset(*mainAsset));
    EXPECT_FALSE(std::filesystem::path(mainAsset->artifactPath).filename().has_extension());

    const auto materialManifest = database.GetArtifactManifestForAssetPath("Assets/Materials/Hero.mat");
    ASSERT_TRUE(materialManifest.has_value());
    EXPECT_TRUE(std::any_of(
        materialManifest->dependencies.begin(),
        materialManifest->dependencies.end(),
        [](const AssetDependencyRecord& dependency)
        {
            return dependency.kind == AssetDependencyKind::PathToGuidMapping &&
                dependency.value == "Assets/Materials/Hero.mat.meta" &&
                !dependency.hashOrVersion.empty();
        }));
    EXPECT_TRUE(std::any_of(
        materialManifest->dependencies.begin(),
        materialManifest->dependencies.end(),
        [](const AssetDependencyRecord& dependency)
        {
            return dependency.kind == AssetDependencyKind::ImporterVersion &&
                dependency.value == "material" &&
                !dependency.hashOrVersion.empty();
        }));

    AssetObjectRecord embeddedTexture;
    embeddedTexture.name = "EmbeddedMask";
    embeddedTexture.artifactType = ArtifactType::Texture;
    embeddedTexture.loaderId = "texture";
    embeddedTexture.serializedPayload = "mask-bytes";

    ASSERT_TRUE(database.AddObjectToAsset(embeddedTexture, "Assets/Materials/Hero.mat"));
    auto allAssets = database.LoadAllAssetsAtPath("Assets/Materials/Hero.mat");
    ASSERT_EQ(allAssets.size(), 2u);
    const auto reloadedMainAsset = database.LoadMainAssetAtPath("Assets/Materials/Hero.mat");
    ASSERT_TRUE(reloadedMainAsset.has_value());
    EXPECT_EQ(
        ReadArtifactPayloadText(reloadedMainAsset->artifactPath, ArtifactType::Material, 1u),
        material.serializedPayload);
    const auto subAsset = database.LoadSubAssetAtPath("Assets/Materials/Hero.mat", "texture:EmbeddedMask");
    ASSERT_TRUE(subAsset.has_value());
    EXPECT_TRUE(database.Contains(*subAsset));
    EXPECT_FALSE(database.IsMainAsset(*subAsset));
    EXPECT_TRUE(database.IsSubAsset(*subAsset));

    const auto uniquePath = database.GenerateUniqueAssetPath("Assets/Materials/Hero.mat");
    EXPECT_EQ(uniquePath, "Assets/Materials/Hero 1.mat");

    ASSERT_TRUE(database.ExtractAsset(*subAsset, "Assets/Textures/ExtractedMask.png"));
    EXPECT_FALSE(database.LoadSubAssetAtPath("Assets/Materials/Hero.mat", "texture:EmbeddedMask").has_value());
    const auto extracted = database.LoadMainAssetAtPath("Assets/Textures/ExtractedMask.png");
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

TEST(AssetDatabaseFacadeTests, LoadsPersistedPrefabArtifactByAssetIdWhenSourcePathIndexIsMissing)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    NLS::Engine::GameObject gameObject("SceneOnlyLamp", "Prefab");
    const auto prefabId = ParseAssetId("e6161616-1616-4616-8616-161616161616");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/SceneOnlyLamp.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    {
        AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.CreateTextAsset(
            created.prefabSourceText,
            "Assets/Prefabs/SceneOnlyLamp.prefab",
            prefabId));
        ASSERT_TRUE(database.ImportAsset("Assets/Prefabs/SceneOnlyLamp.prefab"));
        ASSERT_TRUE(database.LoadPrefabArtifactAtPath(
            "Assets/Prefabs/SceneOnlyLamp.prefab",
            "prefab:SceneOnlyLamp").has_value());
    }

    std::filesystem::remove(root / "Assets" / "Prefabs" / "SceneOnlyLamp.prefab");
    std::filesystem::remove(root / "Assets" / "Prefabs" / "SceneOnlyLamp.prefab.meta");

    {
        const auto manifest = LoadPersistedArtifactManifest(root, prefabId);
        ASSERT_TRUE(manifest.has_value());
        ASSERT_FALSE(manifest->subAssets.empty());
        const auto persistedArtifactPath = manifest->subAssets[0].artifactPath;
        ASSERT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(persistedArtifactPath));
        ASSERT_EQ(persistedArtifactPath.find("Library/Artifacts/"), 0u);
        ASSERT_FALSE(std::filesystem::path(persistedArtifactPath).is_absolute());
        ASSERT_FALSE(std::filesystem::path(persistedArtifactPath).filename().has_extension());
        ASSERT_TRUE(std::filesystem::is_regular_file(root / persistedArtifactPath));
    }

    AssetDatabaseFacade freshDatabase({root});
    ASSERT_TRUE(freshDatabase.Refresh());
    ASSERT_TRUE(freshDatabase.GUIDToAssetPath(prefabId.ToString()).empty());

    auto prefab = freshDatabase.LoadPrefabArtifactByAssetId(prefabId, "prefab:SceneOnlyLamp");
    ASSERT_TRUE(prefab.has_value());
    EXPECT_EQ(prefab->assetId, prefabId);
    EXPECT_FALSE(prefab->generatedModelPrefab);
    EXPECT_FALSE(prefab->Validate().HasErrors());
    EXPECT_EQ(prefab->graph.root.GetGuid(), created.artifact->graph.root.GetGuid());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RejectsPersistedPrefabArtifactOutsidePhysicalArtifactRoot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto prefabId = ParseAssetId("e6262626-2626-4626-8626-262626262626");
    const std::string subAssetKey = "prefab:Escaped";
    NLS::Engine::GameObject gameObject("Escaped", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/Escaped.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    WriteTextFile(root / "Assets" / "Prefabs" / "Escaped.prefab", created.prefabSourceText);
    auto meta = AssetMeta::CreateForAsset(root / "Assets" / "Prefabs" / "Escaped.prefab");
    meta.id = prefabId;
    ASSERT_TRUE(meta.Save(root / "Assets" / "Prefabs" / "Escaped.prefab.meta"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = prefabId;
    manifest.importerId = "prefab";
    manifest.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = subAssetKey;
    manifest.subAssets.push_back(MakeArtifact(
        prefabId,
        subAssetKey,
        ArtifactType::Prefab,
        "prefab",
        "Assets/Escaped.prefab"));
    WritePersistedArtifactManifest(root, manifest);

    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Prefab;
    metadata.schemaName = "prefab-artifact";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = prefabId;
    metadata.subAssetKey = subAssetKey;
    metadata.importerId = "prefab";
    metadata.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    metadata.targetPlatform = "editor";
    WriteBinaryFile(
        root / "Assets" / "Escaped.prefab",
        WriteNativeArtifactContainer(
            std::move(metadata),
            std::vector<uint8_t>(created.prefabSourceText.begin(), created.prefabSourceText.end())));

    std::filesystem::remove(root / "Assets" / "Prefabs" / "Escaped.prefab");
    std::filesystem::remove(root / "Assets" / "Prefabs" / "Escaped.prefab.meta");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.GUIDToAssetPath(prefabId.ToString()).empty());
    EXPECT_FALSE(database.LoadPrefabArtifactByAssetId(prefabId, subAssetKey).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RejectsPersistedPrefabArtifactUnderArbitraryRelativeHashDirectory)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto prefabId = ParseAssetId("e6262626-2727-4627-8627-262626262627");
    const std::string subAssetKey = "prefab:RelativeEscape";
    const std::string blobName =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    NLS::Engine::GameObject gameObject("RelativeEscape", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/RelativeEscape.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    WriteTextFile(root / "Assets" / "Prefabs" / "RelativeEscape.prefab", created.prefabSourceText);
    auto meta = AssetMeta::CreateForAsset(root / "Assets" / "Prefabs" / "RelativeEscape.prefab");
    meta.id = prefabId;
    ASSERT_TRUE(meta.Save(root / "Assets" / "Prefabs" / "RelativeEscape.prefab.meta"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = prefabId;
    manifest.importerId = "prefab";
    manifest.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = subAssetKey;
    manifest.subAssets.push_back(MakeArtifact(
        prefabId,
        subAssetKey,
        ArtifactType::Prefab,
        "prefab",
        "foo/" + blobName));
    WritePersistedArtifactManifest(root, manifest);

    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Prefab;
    metadata.schemaName = "prefab-artifact";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = prefabId;
    metadata.subAssetKey = subAssetKey;
    metadata.importerId = "prefab";
    metadata.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    metadata.targetPlatform = "editor";
    WriteBinaryFile(
        root / "foo" / blobName,
        WriteNativeArtifactContainer(
            std::move(metadata),
            std::vector<uint8_t>(created.prefabSourceText.begin(), created.prefabSourceText.end())));

    std::filesystem::remove(root / "Assets" / "Prefabs" / "RelativeEscape.prefab");
    std::filesystem::remove(root / "Assets" / "Prefabs" / "RelativeEscape.prefab.meta");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.GUIDToAssetPath(prefabId.ToString()).empty());
    EXPECT_FALSE(database.LoadPrefabArtifactByAssetId(prefabId, subAssetKey).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetDatabaseFacadeTests, RejectsPersistedPrefabArtifactSymlinkInsidePhysicalArtifactRoot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDatabaseFacadeRoot();
    const auto outside =
        std::filesystem::temp_directory_path() /
        ("nullus_prefab_artifact_symlink_outside_" + NLS::Guid::New().ToString());
    const auto prefabId = ParseAssetId("e6363636-3636-4636-8636-363636363636");
    const std::string subAssetKey = "prefab:LinkedEscape";
    NLS::Engine::GameObject gameObject("LinkedEscape", "Prefab");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Prefabs/LinkedEscape.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(created.prefabSourceText.empty());

    WriteTextFile(root / "Assets" / "Prefabs" / "LinkedEscape.prefab", created.prefabSourceText);
    auto meta = AssetMeta::CreateForAsset(root / "Assets" / "Prefabs" / "LinkedEscape.prefab");
    meta.id = prefabId;
    ASSERT_TRUE(meta.Save(root / "Assets" / "Prefabs" / "LinkedEscape.prefab.meta"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = prefabId;
    manifest.importerId = "prefab";
    manifest.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = subAssetKey;
    manifest.subAssets.push_back(MakeArtifact(
        prefabId,
        subAssetKey,
        ArtifactType::Prefab,
        "prefab",
        "Library/Artifacts/" + prefabId.ToString() + "/5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d"));
    WritePersistedArtifactManifest(root, manifest);

    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Prefab;
    metadata.schemaName = "prefab-artifact";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = prefabId;
    metadata.subAssetKey = subAssetKey;
    metadata.importerId = "prefab";
    metadata.importerVersion = GetCurrentImporterVersion(AssetType::Prefab);
    metadata.targetPlatform = "editor";
    WriteBinaryFile(
        outside / "5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d",
        WriteNativeArtifactContainer(
            std::move(metadata),
            std::vector<uint8_t>(created.prefabSourceText.begin(), created.prefabSourceText.end())));

    const auto linkPath = root / "Library" / "Artifacts" / prefabId.ToString() / "5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d";
    std::filesystem::create_directories(linkPath.parent_path());
    std::error_code error;
    std::filesystem::create_symlink(outside / "5d4b4d6c2b6c4a6c9b91d90753df2a8d5d4b4d6c2b6c4a6c9b91d90753df2a8d", linkPath, error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "File symlink creation is not available in this environment.";
    }

    std::filesystem::remove(root / "Assets" / "Prefabs" / "LinkedEscape.prefab");
    std::filesystem::remove(root / "Assets" / "Prefabs" / "LinkedEscape.prefab.meta");

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.GUIDToAssetPath(prefabId.ToString()).empty());
    EXPECT_FALSE(database.LoadPrefabArtifactByAssetId(prefabId, subAssetKey).has_value());

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
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
    const std::string meshArtifactPath = "Library/Artifacts/Cube/7e0aaf65f74245f291bdf6a0c3f6c4e8";
    const std::string materialArtifactPath = "Library/Artifacts/47/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae";
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
