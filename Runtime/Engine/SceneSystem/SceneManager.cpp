

#include "SceneSystem/SceneManager.h"
#include "Components/LightComponent.h"
#include "Components/SkyBoxComponent.h"
#include "Components/CameraComponent.h"
#include "Components/TransformComponent.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Serialize/ObjectGraphBinaryCache.h"
#include "Profiling/PerformanceStageStats.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

namespace
{
    constexpr uint8_t kSceneObjectGraphCacheMagic[] = {'N', 'S', 'G', 'C'};
    constexpr uint32_t kSceneObjectGraphCacheVersion = 1u;

    struct SceneObjectGraphSourceStamp
    {
        uint64_t fileSize = 0u;
        int64_t writeTimeTicks = 0;
    };

    bool operator==(const SceneObjectGraphSourceStamp& left, const SceneObjectGraphSourceStamp& right)
    {
        return left.fileSize == right.fileSize &&
            left.writeTimeTicks == right.writeTimeTicks;
    }

    void AppendSceneManagerTrace(const char* message)
    {
        (void)message;
    }

    bool WriteTextFileAtomically(const std::filesystem::path& path, const std::string& text)
    {
        std::error_code error;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), error);

        const auto temporaryPath = path.string() + ".tmp";
        {
            std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;

            file << text;
            if (!file.good())
                return false;
        }

        if (std::filesystem::exists(path, error))
            std::filesystem::remove(path, error);

        std::filesystem::rename(temporaryPath, path, error);
        if (error)
        {
            error.clear();
            std::filesystem::copy_file(temporaryPath, path, std::filesystem::copy_options::overwrite_existing, error);
            std::filesystem::remove(temporaryPath, error);
        }

        return !error;
    }

    std::optional<std::string> ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt;

        std::ostringstream stream;
        stream << file.rdbuf();
        return stream.str();
    }

    std::optional<std::vector<uint8_t>> ReadBinaryFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt;

        file.seekg(0, std::ios::end);
        const auto size = file.tellg();
        if (size < 0)
            return std::nullopt;
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!bytes.empty())
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file.good() && !file.eof())
            return std::nullopt;
        return bytes;
    }

    bool WriteBinaryFileAtomically(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
    {
        std::error_code error;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), error);

        const auto temporaryPath = path.string() + ".tmp";
        {
            std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            if (!bytes.empty())
                file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file.good())
                return false;
        }

        if (std::filesystem::exists(path, error))
            std::filesystem::remove(path, error);

        std::filesystem::rename(temporaryPath, path, error);
        if (error)
        {
            error.clear();
            std::filesystem::copy_file(temporaryPath, path, std::filesystem::copy_options::overwrite_existing, error);
            std::filesystem::remove(temporaryPath, error);
        }

        return !error;
    }

    uint64_t HashPathForCacheKey(const std::string& text)
    {
        uint64_t hash = 14695981039346656037ull;
        for (const unsigned char value : text)
        {
            hash ^= value;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::string ToFixedHex(const uint64_t value)
    {
        constexpr char kDigits[] = "0123456789abcdef";
        std::string text(16u, '0');
        for (size_t index = 0u; index < text.size(); ++index)
        {
            const auto shift = static_cast<uint32_t>((text.size() - index - 1u) * 4u);
            text[index] = kDigits[(value >> shift) & 0x0full];
        }
        return text;
    }

    std::optional<SceneObjectGraphSourceStamp> GetSceneObjectGraphSourceStamp(const std::filesystem::path& path)
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error)
            return std::nullopt;

        const auto writeTime = std::filesystem::last_write_time(path, error);
        if (error)
            return std::nullopt;

        return SceneObjectGraphSourceStamp{
            size,
            static_cast<int64_t>(writeTime.time_since_epoch().count())
        };
    }

    std::filesystem::path ResolveSceneObjectGraphCachePath(const std::filesystem::path& scenePath)
    {
        std::error_code error;
        const auto absolutePath = std::filesystem::absolute(scenePath, error).lexically_normal();
        const auto key = ToFixedHex(HashPathForCacheKey(absolutePath.generic_string()));

        for (auto parent = absolutePath.parent_path(); !parent.empty(); parent = parent.parent_path())
        {
            if (parent.filename() == "Assets")
            {
                return parent.parent_path() /
                    "Library" /
                    "SceneObjectGraphCache" /
                    (key + ".nogc");
            }

            if (parent == parent.parent_path())
                break;
        }

        return absolutePath.parent_path() /
            ".nullus_scene_cache" /
            (absolutePath.filename().string() + "." + key + ".nogc");
    }

    class SceneObjectGraphCacheReader
    {
    public:
        SceneObjectGraphCacheReader(const uint8_t* input, const size_t inputSize)
            : data(input)
            , size(inputSize)
        {
        }

        std::optional<NLS::Engine::Serialize::ObjectGraphDocument> Read(const SceneObjectGraphSourceStamp& stamp)
        {
            if (!ReadMagic())
                return std::nullopt;

            uint32_t version = 0u;
            uint64_t sourceSize = 0u;
            int64_t sourceWriteTime = 0;
            uint32_t payloadSize = 0u;
            if (!ReadU32(version) ||
                version != kSceneObjectGraphCacheVersion ||
                !ReadU64(sourceSize) ||
                !ReadI64(sourceWriteTime) ||
                !ReadU32(payloadSize) ||
                sourceSize != stamp.fileSize ||
                sourceWriteTime != stamp.writeTimeTicks ||
                Remaining() != payloadSize)
            {
                return std::nullopt;
            }

            auto document = NLS::Engine::Serialize::ObjectGraphBinaryCache::Read(data + position, payloadSize);
            if (!document.has_value() || document->Validate().HasErrors())
                return std::nullopt;
            position += payloadSize;
            return document;
        }

    private:
        bool ReadMagic()
        {
            if (Remaining() < sizeof(kSceneObjectGraphCacheMagic))
                return false;
            if (std::memcmp(data + position, kSceneObjectGraphCacheMagic, sizeof(kSceneObjectGraphCacheMagic)) != 0)
                return false;
            position += sizeof(kSceneObjectGraphCacheMagic);
            return true;
        }

        bool ReadU32(uint32_t& value)
        {
            if (Remaining() < 4u)
                return false;
            value =
                static_cast<uint32_t>(data[position]) |
                (static_cast<uint32_t>(data[position + 1u]) << 8u) |
                (static_cast<uint32_t>(data[position + 2u]) << 16u) |
                (static_cast<uint32_t>(data[position + 3u]) << 24u);
            position += 4u;
            return true;
        }

        bool ReadU64(uint64_t& value)
        {
            if (Remaining() < 8u)
                return false;
            value =
                static_cast<uint64_t>(data[position]) |
                (static_cast<uint64_t>(data[position + 1u]) << 8u) |
                (static_cast<uint64_t>(data[position + 2u]) << 16u) |
                (static_cast<uint64_t>(data[position + 3u]) << 24u) |
                (static_cast<uint64_t>(data[position + 4u]) << 32u) |
                (static_cast<uint64_t>(data[position + 5u]) << 40u) |
                (static_cast<uint64_t>(data[position + 6u]) << 48u) |
                (static_cast<uint64_t>(data[position + 7u]) << 56u);
            position += 8u;
            return true;
        }

        bool ReadI64(int64_t& value)
        {
            uint64_t raw = 0u;
            if (!ReadU64(raw))
                return false;
            value = static_cast<int64_t>(raw);
            return true;
        }

        size_t Remaining() const
        {
            return size - position;
        }

        const uint8_t* data = nullptr;
        size_t size = 0u;
        size_t position = 0u;
    };

    void WriteU32(std::vector<uint8_t>& bytes, const uint32_t value)
    {
        for (uint32_t shift = 0u; shift < 32u; shift += 8u)
            bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }

    void WriteU64(std::vector<uint8_t>& bytes, const uint64_t value)
    {
        for (uint32_t shift = 0u; shift < 64u; shift += 8u)
            bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }

    void WriteI64(std::vector<uint8_t>& bytes, const int64_t value)
    {
        WriteU64(bytes, static_cast<uint64_t>(value));
    }

    std::optional<NLS::Engine::Serialize::ObjectGraphDocument> ReadSceneObjectGraphCache(
        const std::filesystem::path& scenePath,
        const SceneObjectGraphSourceStamp& stamp,
        uint64_t& cacheByteCount)
    {
        cacheByteCount = 0u;
        const auto cachePath = ResolveSceneObjectGraphCachePath(scenePath);
        const auto bytes = ReadBinaryFile(cachePath);
        if (!bytes.has_value())
            return std::nullopt;

        cacheByteCount = bytes->size();
        return SceneObjectGraphCacheReader(bytes->data(), bytes->size()).Read(stamp);
    }

    bool WriteSceneObjectGraphCache(
        const std::filesystem::path& scenePath,
        const SceneObjectGraphSourceStamp& stamp,
        const NLS::Engine::Serialize::ObjectGraphDocument& document)
    {
        const auto payload = NLS::Engine::Serialize::ObjectGraphBinaryCache::Write(document);
        if (payload.empty() || payload.size() > std::numeric_limits<uint32_t>::max())
            return false;

        std::vector<uint8_t> bytes;
        bytes.reserve(sizeof(kSceneObjectGraphCacheMagic) + 4u + 8u + 8u + 4u + payload.size());
        bytes.insert(bytes.end(), std::begin(kSceneObjectGraphCacheMagic), std::end(kSceneObjectGraphCacheMagic));
        WriteU32(bytes, kSceneObjectGraphCacheVersion);
        WriteU64(bytes, stamp.fileSize);
        WriteI64(bytes, stamp.writeTimeTicks);
        WriteU32(bytes, static_cast<uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());

        return WriteBinaryFileAtomically(ResolveSceneObjectGraphCachePath(scenePath), bytes);
    }

    void ReportSceneLoadProgress(
        const NLS::Engine::SceneSystem::SceneLoadProgressCallback& callback,
        const float progress,
        const std::string& message)
    {
        if (callback)
            callback({std::clamp(progress, 0.0f, 1.0f), message});
    }
}

using namespace NLS;
using namespace NLS::Engine::SceneSystem;

SceneManager::SceneManager(const std::string& p_sceneRootFolder)
    : m_sceneRootFolder(p_sceneRootFolder)
{
    LoadEmptyScene();
}

SceneManager::~SceneManager()
{
    UnloadCurrentScene();
}

void SceneManager::Update()
{
    if (m_delayedLoadCall)
    {
        m_delayedLoadCall();
        m_delayedLoadCall = 0;
    }
}

void SceneManager::LoadAndPlayDelayed(const std::string& p_path, bool p_absolute)
{
    m_delayedLoadCall = [this, p_path, p_absolute]
    {
        std::string previousSourcePath = GetCurrentSceneSourcePath();
        LoadScene(p_path, p_absolute);
        StoreCurrentSceneSourcePath(previousSourcePath);
        GetCurrentScene()->Play();
    };
}

void SceneManager::LoadEmptyScene()
{
    UnloadCurrentScene();
    m_lastLoadedSceneDocument.reset();

    m_currentScene = new Scene();
    MarkCurrentSceneClean();

    SceneLoadEvent.Invoke();
}

void SceneManager::LoadEmptyLightedScene()
{
    AppendSceneManagerTrace("LoadEmptyLightedScene begin");
    UnloadCurrentScene();
    m_lastLoadedSceneDocument.reset();

    m_currentScene = new Scene();
    MarkCurrentSceneClean();

    SceneLoadEvent.Invoke();

    auto& directionalLightGo = m_currentScene->CreateGameObject("Directional Light");
    auto directionalLight = directionalLightGo.AddComponent<Engine::Components::LightComponent>();
    if (directionalLight)
    {
        directionalLight->SetLightType(Render::Settings::ELightType::DIRECTIONAL);
        directionalLight->SetIntensity(0.75f);
    }
    if (auto tr = directionalLightGo.GetTransform())
    {
        tr->SetLocalPosition({0.0f, 10.0f, 0.0f});
        tr->SetLocalRotation(Maths::Quaternion({60.0f, 40.0f, 0.0f}));
    }

    auto& ambientLightGo = m_currentScene->CreateGameObject("Ambient Light");
    auto ambientLight = ambientLightGo.AddComponent<Engine::Components::LightComponent>();
    if (ambientLight)
    {
        ambientLight->SetLightType(Render::Settings::ELightType::AMBIENT_SPHERE);
        ambientLight->SetRange(10000.0f);
        ambientLight->SetIntensity(0.1f);
    }

    auto& skyboxGo = m_currentScene->CreateGameObject("Skybox");
    AppendSceneManagerTrace("adding SkyBoxComponent");
    skyboxGo.AddComponent<Engine::Components::SkyBoxComponent>();
    AppendSceneManagerTrace("SkyBoxComponent added");

    auto& camera = m_currentScene->CreateGameObject("Main Camera");
    if (auto cameraComponent = camera.AddComponent<Engine::Components::CameraComponent>())
    {
        cameraComponent->SetClearColor({0.08f, 0.12f, 0.2f});
        cameraComponent->SetFrustumGeometryCulling(true);
    }
    if (auto tr = camera.GetTransform())
    {
        tr->SetLocalPosition({0.0f, 0.0f, 0.0f});
        tr->SetLocalRotation(Maths::Quaternion::Identity);
    }

    AppendSceneManagerTrace("LoadEmptyLightedScene end");
}

bool SceneManager::LoadScene(const std::string& p_path, bool p_absolute)
{
    return LoadScene(p_path, p_absolute, {});
}

bool SceneManager::LoadScene(
    const std::string& p_path,
    bool p_absolute,
    const SceneLoadProgressCallback& p_progressCallback)
{
    namespace Profiling = NLS::Base::Profiling;

    std::string completePath = (p_absolute ? "" : m_sceneRootFolder) + p_path;
    const auto scenePath = std::filesystem::path(completePath);
    const auto sourceStamp = GetSceneObjectGraphSourceStamp(scenePath);
    if (!sourceStamp.has_value())
        return false;

    ReportSceneLoadProgress(p_progressCallback, 0.08f, "Checking scene object graph cache");
    std::optional<Engine::Serialize::ObjectGraphDocument> document;
    {
        Profiling::PerformanceStageScope scope(
            Profiling::PerformanceStageDomain::Unknown,
            "SceneLoad.ReadObjectGraphCache",
            Profiling::PerformanceStageThread::Main);
        uint64_t cacheByteCount = 0u;
        document = ReadSceneObjectGraphCache(scenePath, *sourceStamp, cacheByteCount);
        scope.AddCounter("cacheByteCount", cacheByteCount);
        scope.AddCounter("cacheHitCount", document.has_value() ? 1u : 0u);
    }

    std::optional<std::string> fileText;
    if (!document.has_value())
    {
        ReportSceneLoadProgress(p_progressCallback, 0.02f, "Reading scene file");
        {
            Profiling::PerformanceStageScope scope(
                Profiling::PerformanceStageDomain::Unknown,
                "SceneLoad.ReadSceneFile",
                Profiling::PerformanceStageThread::Main);
            fileText = ReadTextFile(completePath);
            if (fileText.has_value())
                scope.AddCounter("byteCount", fileText->size());
        }
        if (!fileText.has_value())
            return false;
    }

    if (!document.has_value())
    {
        ReportSceneLoadProgress(p_progressCallback, 0.12f, "Parsing scene object graph");
        {
            Profiling::PerformanceStageScope scope(
                Profiling::PerformanceStageDomain::Unknown,
                "SceneLoad.ParseObjectGraph",
                Profiling::PerformanceStageThread::Main);
            document = Engine::Serialize::ObjectGraphReader::Read(*fileText);
            if (document.has_value())
                scope.AddCounter("objectCount", document->objects.size());
        }
    }
    m_lastLoadedSceneDocument.reset();
    if (!document.has_value() || document->format != "Nullus.ObjectGraph.Scene")
        return false;

    if (fileText.has_value())
    {
        Profiling::PerformanceStageScope scope(
            Profiling::PerformanceStageDomain::Unknown,
            "SceneLoad.WriteObjectGraphCache",
            Profiling::PerformanceStageThread::Main);
        const auto postParseStamp = GetSceneObjectGraphSourceStamp(scenePath);
        const bool canWriteCache = postParseStamp.has_value() && *postParseStamp == *sourceStamp;
        scope.AddCounter("cacheWriteSuccessCount", canWriteCache && WriteSceneObjectGraphCache(scenePath, *sourceStamp, *document) ? 1u : 0u);
        scope.AddCounter("objectCount", document->objects.size());
    }

    ReportSceneLoadProgress(p_progressCallback, 0.20f, "Unloading previous scene");
    {
        Profiling::PerformanceStageScope scope(
            Profiling::PerformanceStageDomain::Unknown,
            "SceneLoad.UnloadPreviousScene",
            Profiling::PerformanceStageThread::Main);
        UnloadCurrentScene();
    }

    Engine::Serialize::LoadPolicy loadPolicy;
    loadPolicy.deferAssetReferenceResolution = true;
    loadPolicy.invalidReferencePolicy = Engine::Serialize::InvalidReferencePolicy::Preserve;
    Engine::Serialize::SceneInstantiationResult sceneResult;
    {
        Profiling::PerformanceStageScope scope(
            Profiling::PerformanceStageDomain::Unknown,
            "SceneLoad.InstantiateScene",
            Profiling::PerformanceStageThread::Main);
        scope.AddCounter("objectCount", document->objects.size());
        scope.AddCounter("documentValidationCount", 1u);
        Engine::Serialize::InstantiationProgressCallback instantiationProgressCallback;
        if (p_progressCallback)
        {
            instantiationProgressCallback = [&p_progressCallback](
                const Engine::Serialize::InstantiationProgress& progress)
            {
                ReportSceneLoadProgress(
                    p_progressCallback,
                    0.20f + progress.normalizedProgress * 0.70f,
                    progress.message);
            };
        }
        sceneResult = Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(
            *document,
            loadPolicy,
            instantiationProgressCallback);
        scope.AddCounter("progressCallbackEnabledCount", instantiationProgressCallback ? 1u : 0u);
        scope.AddCounter("successCount", sceneResult.scene ? 1u : 0u);
    }
    auto& scene = sceneResult.scene;
    if (!scene)
    {
        m_currentScene = new Scene();
        m_lastLoadedSceneDocument.reset();
        {
            Profiling::PerformanceStageScope scope(
                Profiling::PerformanceStageDomain::Unknown,
                "SceneLoad.SceneLoadEvent",
                Profiling::PerformanceStageThread::Main);
            SceneLoadEvent.Invoke();
        }
        MarkCurrentSceneClean();
        return false;
    }

    m_currentScene = scene.release();
    m_lastLoadedSceneDocument = std::move(*document);
    ReportSceneLoadProgress(p_progressCallback, 0.92f, "Activating loaded scene");
    {
        Profiling::PerformanceStageScope scope(
            Profiling::PerformanceStageDomain::Unknown,
            "SceneLoad.SceneLoadEvent",
            Profiling::PerformanceStageThread::Main);
        SceneLoadEvent.Invoke();
    }
    {
        Profiling::PerformanceStageScope scope(
            Profiling::PerformanceStageDomain::Unknown,
            "SceneLoad.StoreScenePath",
            Profiling::PerformanceStageThread::Main);
        StoreCurrentSceneSourcePath(completePath);
        MarkCurrentSceneClean();
    }
    ReportSceneLoadProgress(p_progressCallback, 1.0f, "Scene loaded");
    return true;
}

bool SceneManager::SaveCurrentScene(const std::string& p_path)
{
    if (!m_currentScene || p_path.empty())
        return false;

    if (!SaveSceneToPath(*m_currentScene, p_path))
        return false;

    StoreCurrentSceneSourcePath(p_path);
    MarkCurrentSceneClean();
    return true;
}

bool SceneManager::SaveSceneToPath(const Scene& p_scene, const std::string& p_path)
{
    if (p_path.empty())
        return false;

    const auto document = Engine::Serialize::ObjectGraphSerializer::SerializeScene(p_scene);
    if (document.Validate().HasErrors())
        return false;

    return WriteTextFileAtomically(p_path, Engine::Serialize::ObjectGraphWriter::Write(document));
}

// bool SceneManager::LoadSceneFromMemory(tinyxml2::XMLDocument& p_doc)
//{
//	if (!p_doc.Error())
//	{
//		tinyxml2::XMLNode* root = p_doc.FirstChild();
//		if (root)
//		{
//			tinyxml2::XMLNode* sceneNode = root->FirstChildElement("scene");
//			if (sceneNode)
//			{
//				LoadEmptyScene();
//				m_currentScene->OnDeserialize(p_doc, sceneNode);
//				return true;
//			}
//		}
//	}
//
//	Windowing::Dialogs::MessageBox message("Scene loading failed", "The scene you are trying to load was not found or corrupted", Windowing::Dialogs::MessageBox::EMessageType::ERROR, Windowing::Dialogs::MessageBox::EButtonLayout::OK, true);
//	return false;
// }

void SceneManager::UnloadCurrentScene()
{
    m_lastLoadedSceneDocument.reset();
    if (m_currentScene)
    {
        delete m_currentScene;
        m_currentScene = nullptr;
        SceneUnloadEvent.Invoke();
    }

    ForgetCurrentSceneSourcePath();
    MarkCurrentSceneClean();
}

bool SceneManager::HasCurrentScene() const
{
    return m_currentScene;
}

Engine::SceneSystem::Scene* SceneManager::GetCurrentScene() const
{
    return m_currentScene;
}

const Engine::Serialize::ObjectGraphDocument* SceneManager::GetLastLoadedSceneDocument() const
{
    return m_lastLoadedSceneDocument ? &*m_lastLoadedSceneDocument : nullptr;
}

std::string SceneManager::GetCurrentSceneSourcePath() const
{
    return m_currentSceneSourcePath;
}

bool SceneManager::IsCurrentSceneLoadedFromDisk() const
{
    return m_currentSceneLoadedFromPath;
}

void SceneManager::StoreCurrentSceneSourcePath(const std::string& p_path)
{
    if (m_currentSceneSourcePath == p_path && m_currentSceneLoadedFromPath)
        return;

    m_currentSceneSourcePath = p_path;
    m_currentSceneLoadedFromPath = true;
    CurrentSceneSourcePathChangedEvent.Invoke(m_currentSceneSourcePath);
}

void SceneManager::ForgetCurrentSceneSourcePath()
{
    if (m_currentSceneSourcePath.empty() && !m_currentSceneLoadedFromPath)
        return;

    m_currentSceneSourcePath = "";
    m_currentSceneLoadedFromPath = false;
    CurrentSceneSourcePathChangedEvent.Invoke(m_currentSceneSourcePath);
}

void SceneManager::MarkCurrentSceneDirty()
{
    if (m_currentScene)
        m_currentScene->MarkRenderContentChanged();

    if (m_currentSceneDirty)
        return;

    m_currentSceneDirty = true;
    CurrentSceneDirtyStateChangedEvent.Invoke(m_currentSceneDirty);
}

void SceneManager::MarkCurrentSceneClean()
{
    if (!m_currentSceneDirty)
        return;

    m_currentSceneDirty = false;
    CurrentSceneDirtyStateChangedEvent.Invoke(m_currentSceneDirty);
}

bool SceneManager::HasUnsavedSceneChanges() const
{
    return m_currentSceneDirty;
}
