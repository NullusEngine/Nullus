

#include "SceneSystem/SceneManager.h"
#include "Components/LightComponent.h"
#include "Components/SkyBoxComponent.h"
#include "Components/CameraComponent.h"
#include "Components/TransformComponent.h"
#include "ResourceManagement/TextureManager.h"
#include "ServiceLocator.h"
#include "Serialize/Serializer.h"

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

    SceneLoadEvent.Invoke();
}

void SceneManager::LoadEmptyLightedScene()
{
    UnloadCurrentScene();

    m_currentScene = new Scene();

    SceneLoadEvent.Invoke();

    auto& directionalLightGo = m_currentScene->CreateGameObject("Directional Light");
    auto directionalLight = directionalLightGo.AddComponent<Engine::Components::LightComponent>();
    directionalLight->SetLightType(Render::Settings::ELightType::DIRECTIONAL);
    directionalLight->SetIntensity(0.75f);
    directionalLightGo.GetTransform()->SetLocalPosition({0.0f, 10.0f, 0.0f});
    directionalLightGo.GetTransform()->SetLocalRotation(Maths::Quaternion({120.0f, -40.0f, 0.0f}));

    auto& ambientLightGo = m_currentScene->CreateGameObject("Ambient Light");
    auto ambientLight = ambientLightGo.AddComponent<Engine::Components::LightComponent>();
    ambientLight->SetLightType(Render::Settings::ELightType::AMBIENT_SPHERE);
    ambientLight->SetRadius(10000.0f);

    auto& skyboxGo = m_currentScene->CreateGameObject("Skybox");
    auto skybox = skyboxGo.AddComponent<Engine::Components::SkyBoxComponent>();
    auto cube = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager).CreateCubeMap({":Textures\\skybox\\right.jpg", ":Textures\\skybox\\left.jpg", ":Textures\\skybox\\top.jpg", ":Textures\\skybox\\bottom.jpg", ":Textures\\skybox\\front.jpg", ":Textures\\skybox\\back.jpg"});
    // auto cube = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager).CreateCubeMap({
    //	":Textures\\Cubemap\\skyrender0001.png",
    //	":Textures\\Cubemap\\skyrender0004.png",
    //	":Textures\\Cubemap\\skyrender0003.png",
    //	":Textures\\Cubemap\\skyrender0006.png",
    //	":Textures\\Cubemap\\skyrender0005.png",
    //	":Textures\\Cubemap\\skyrender0002.png"});
    skybox->SetCubeMap(cube);

    auto& camera = m_currentScene->CreateGameObject("Main Camera");
    camera.AddComponent<Engine::Components::CameraComponent>();
    camera.GetTransform()->SetLocalPosition({0.0f, 3.0f, 8.0f});
    camera.GetTransform()->SetLocalRotation(Maths::Quaternion({160.0f, 0.0f, 0.0f}));
}

bool SceneManager::LoadScene(const std::string& p_path, bool p_absolute)
{
    std::string completePath = (p_absolute ? "" : m_sceneRootFolder) + p_path;
    ObjectView scene = Mngr.New(Type_of<Scene>);
    Serializer::Instance()->DeserializeFromFile(scene, completePath);
    UnloadCurrentScene();
    m_currentScene = scene.AsPtr<Scene>();
    SceneLoadEvent.Invoke();
    StoreCurrentSceneSourcePath(completePath);
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
    m_currentSceneSourcePath = p_path;
    m_currentSceneLoadedFromPath = true;
    CurrentSceneSourcePathChangedEvent.Invoke(m_currentSceneSourcePath);
}

void SceneManager::ForgetCurrentSceneSourcePath()
{
    m_currentSceneSourcePath = "";
    m_currentSceneLoadedFromPath = false;
    CurrentSceneSourcePathChangedEvent.Invoke(m_currentSceneSourcePath);
}
