

#include "SceneSystem/SceneManager.h"
#include "Components/LightComponent.h"
#include "Components/SkyBoxComponent.h"
#include "Components/CameraComponent.h"
#include "Components/TransformComponent.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <memory>
#include <sstream>

namespace
{
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
        ambientLight->SetRadius(10000.0f);
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
    std::string completePath = (p_absolute ? "" : m_sceneRootFolder) + p_path;

    ReportSceneLoadProgress(p_progressCallback, 0.02f, "Reading scene file");
    const auto fileText = ReadTextFile(completePath);
    if (!fileText.has_value())
        return false;

    ReportSceneLoadProgress(p_progressCallback, 0.12f, "Parsing scene object graph");
    auto document = Engine::Serialize::ObjectGraphReader::Read(*fileText);
    m_lastLoadedSceneDocument.reset();
    if (!document.has_value() || document->format != "Nullus.ObjectGraph.Scene")
        return false;

    ReportSceneLoadProgress(p_progressCallback, 0.20f, "Unloading previous scene");
    UnloadCurrentScene();

    Engine::Serialize::LoadPolicy loadPolicy;
    loadPolicy.deferAssetReferenceResolution = true;
    loadPolicy.invalidReferencePolicy = Engine::Serialize::InvalidReferencePolicy::Preserve;
    auto sceneResult = Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(
        *document,
        loadPolicy,
        [&p_progressCallback](const Engine::Serialize::InstantiationProgress& progress)
        {
            ReportSceneLoadProgress(
                p_progressCallback,
                0.20f + progress.normalizedProgress * 0.70f,
                progress.message);
        });
    auto& scene = sceneResult.scene;
    if (!scene)
    {
        m_currentScene = new Scene();
        m_lastLoadedSceneDocument.reset();
        SceneLoadEvent.Invoke();
        MarkCurrentSceneClean();
        return false;
    }

    m_currentScene = scene.release();
    m_lastLoadedSceneDocument = std::move(*document);
    ReportSceneLoadProgress(p_progressCallback, 0.92f, "Activating loaded scene");
    SceneLoadEvent.Invoke();
    StoreCurrentSceneSourcePath(completePath);
    MarkCurrentSceneClean();
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
