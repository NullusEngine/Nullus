

#include "SceneSystem/SceneManager.h"
#include "Components/LightComponent.h"
#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Components/SkyBoxComponent.h"
#include "Components/CameraComponent.h"
#include "Components/TransformComponent.h"
#include "ResourceManagement/MaterialManager.h"
#include "ResourceManagement/ModelManager.h"
#include "ResourceManagement/TextureManager.h"
#include "ServiceLocator.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"
#include <filesystem>
#include <fstream>
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

    m_currentScene = new Scene();
    MarkCurrentSceneClean();

    SceneLoadEvent.Invoke();
}

void SceneManager::LoadEmptyLightedScene()
{
    AppendSceneManagerTrace("LoadEmptyLightedScene begin");
    UnloadCurrentScene();

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
    }
    if (auto tr = camera.GetTransform())
    {
        tr->SetLocalPosition({0.0f, 0.0f, 0.0f});
        tr->SetLocalRotation(Maths::Quaternion::Identity);
    }

    if (Core::ServiceLocator::Contains<Core::ResourceManagement::ModelManager>() &&
        Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
    {
        auto& validationCube = m_currentScene->CreateGameObject("Validation Cube");
        if (auto tr = validationCube.GetTransform())
        {
            tr->SetLocalPosition({0.0f, 0.0f, 5.0f});
            tr->SetLocalScale({1.5f, 1.5f, 1.5f});
        }

        auto* meshRenderer = validationCube.AddComponent<Engine::Components::MeshRenderer>();
        auto* materialRenderer = validationCube.AddComponent<Engine::Components::MaterialRenderer>();
        if (meshRenderer != nullptr)
        {
            meshRenderer->SetModel(
                NLS_SERVICE(Core::ResourceManagement::ModelManager)[":Models\\Cube.fbx"]);
        }
        if (materialRenderer != nullptr)
        {
            if (auto* defaultMaterial =
                    NLS_SERVICE(Core::ResourceManagement::MaterialManager)[":Materials\\Default.mat"];
                defaultMaterial != nullptr)
            {
                materialRenderer->FillWithMaterial(*defaultMaterial);
            }
        }
    }

    AppendSceneManagerTrace("LoadEmptyLightedScene end");
}

bool SceneManager::LoadScene(const std::string& p_path, bool p_absolute)
{
    std::string completePath = (p_absolute ? "" : m_sceneRootFolder) + p_path;

    const auto fileText = ReadTextFile(completePath);
    if (!fileText.has_value())
        return false;

    const auto document = Engine::Serialize::ObjectGraphReader::Read(*fileText);
    if (!document.has_value() || document->format != "Nullus.ObjectGraph.Scene")
        return false;

    UnloadCurrentScene();

    auto scene = Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(*document);
    if (!scene)
    {
        m_currentScene = new Scene();
        SceneLoadEvent.Invoke();
        MarkCurrentSceneClean();
        return false;
    }

    m_currentScene = scene.release();
    SceneLoadEvent.Invoke();
    StoreCurrentSceneSourcePath(completePath);
    MarkCurrentSceneClean();
    return true;
}

bool SceneManager::SaveCurrentScene(const std::string& p_path)
{
    if (!m_currentScene || p_path.empty())
        return false;

    const auto document = Engine::Serialize::ObjectGraphSerializer::SerializeScene(*m_currentScene);
    if (document.Validate().HasErrors())
        return false;

    const auto text = Engine::Serialize::ObjectGraphWriter::Write(document);
    if (!WriteTextFileAtomically(p_path, text))
        return false;

    StoreCurrentSceneSourcePath(p_path);
    MarkCurrentSceneClean();
    return true;
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
