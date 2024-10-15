#include <filesystem>
#include <iostream>
#include <fstream>

#include <Debug/Logger.h>

#include <Components/MeshRenderer.h>
#include <Components/MaterialRenderer.h>


#include <Windowing/Dialogs/OpenFileDialog.h>
#include <Windowing/Dialogs/SaveFileDialog.h>
#include <Windowing/Dialogs/MessageBox.h>

#include <Utils/PathParser.h>
#include <Utils/String.h>
#include <Utils/SystemCalls.h>

#include "Core/EditorActions.h"
#include "Components/TransformComponent.h"
#include "Panels/Inspector.h"
#include "Panels/SceneView.h"
#include "ResourceManagement/ModelManager.h"
#include "Resource/Actor/ActorManager.h"
#include "ServiceLocator.h"
#include "Panels/AssetView.h"
#include "Panels/MaterialEditor.h"
#include "Serialize/Serializer.h"
// #include "Panels/SceneView.h"
// #include "Panels/AssetView.h"
// #include "Panels/GameView.h"
// #include "Panels/Inspector.h"
// #include "Panels/ProjectSettings.h"
// #include "Panels/MaterialEditor.h"

using namespace NLS;
Editor::Core::EditorActions::EditorActions(Context& p_context, PanelsManager& p_panelsManager)
    : m_context(p_context), m_panelsManager(p_panelsManager)
{
    NLS::Core::ServiceLocator::Provide<Editor::Core::EditorActions>(*this);

    m_context.sceneManager.CurrentSceneSourcePathChangedEvent += [this](const std::string& p_newPath)
    {
        std::string titleExtra = " - " + (p_newPath.empty() ? "Untitled Scene" : GetResourcePath(p_newPath));
        m_context.window->SetTitle(m_context.windowSettings.title + titleExtra);
    };
}

void Editor::Core::EditorActions::LoadEmptyScene()
{
    if (GetCurrentEditorMode() != EEditorMode::EDIT)
        StopPlaying();

    m_context.sceneManager.LoadEmptyLightedScene();
    NLS_LOG_INFO("New scene created");
}

void Editor::Core::EditorActions::SaveCurrentSceneTo(const std::string& p_path)
{
    m_context.sceneManager.StoreCurrentSceneSourcePath(p_path);
    Serializer::Instance()->SerializeToFile({Type_of<NLS::Engine::SceneSystem::Scene>, m_context.sceneManager.GetCurrentScene()}, p_path);
}

void Editor::Core::EditorActions::LoadSceneFromDisk(const std::string& p_path, bool p_absolute)
{
    if (GetCurrentEditorMode() != EEditorMode::EDIT)
        StopPlaying();

    m_context.sceneManager.LoadScene(p_path, p_absolute);
    NLS_LOG_INFO("Scene loaded from disk: " + m_context.sceneManager.GetCurrentSceneSourcePath());
    m_panelsManager.GetPanelAs<Editor::Panels::SceneView>("Scene View").Focus();
}

bool Editor::Core::EditorActions::IsCurrentSceneLoadedFromDisk() const
{
    return m_context.sceneManager.IsCurrentSceneLoadedFromDisk();
}

void Editor::Core::EditorActions::SaveSceneChanges()
{
    if (IsCurrentSceneLoadedFromDisk())
    {
        SaveCurrentSceneTo(m_context.sceneManager.GetCurrentSceneSourcePath());
        NLS_LOG_INFO("Current scene saved to: " + m_context.sceneManager.GetCurrentSceneSourcePath());
    }
    else
    {
        SaveAs();
    }
}

void Editor::Core::EditorActions::SaveAs()
{
    Dialogs::SaveFileDialog dialog("New Scene", m_context.projectAssetsPath + "New Scene", {"Nullus Scene", "*.scene"});

    if (!dialog.Result().empty())
    {
        if (std::filesystem::exists(dialog.Result()))
        {
            Dialogs::MessageBox message("File already exists!", "The file \"" + dialog.Result() + "\" already exists.\n\nUsing this file as the new home for your scene will erase any content stored in this file.\n\nAre you ok with that?", Dialogs::MessageBox::EMessageType::WARNING, Dialogs::MessageBox::EButtonLayout::YES_NO);
            switch (message.GetUserAction())
            {
                case Dialogs::MessageBox::EUserAction::YES:
                    break;
                case Dialogs::MessageBox::EUserAction::NO:
                    return;
            }
        }

        SaveCurrentSceneTo(dialog.Result());
        NLS_LOG_INFO("Current scene saved to: " + dialog.Result());
    }
}


std::optional<std::string> Editor::Core::EditorActions::SelectBuildFolder()
{
    Dialogs::SaveFileDialog dialog("Build location", "", {"Game Build", ".."});
    if (!dialog.Result().empty())
    {
        std::string result = dialog.Result();
        result = std::string(result.data(), result.data() + result.size() - std::string("..").size()) + "\\"; // remove auto extension
        if (!std::filesystem::exists(result))
            return result;
        else
        {
            Dialogs::MessageBox message("Folder already exists!", "The folder \"" + result + "\" already exists.\n\nPlease select another location and try again", Dialogs::MessageBox::EMessageType::WARNING, Dialogs::MessageBox::EButtonLayout::OK);
            return {};
        }
    }
    else
    {
        return {};
    }
}

void Editor::Core::EditorActions::Build(bool p_autoRun, bool p_tempFolder)
{
    std::string destinationFolder;

    if (p_tempFolder)
    {
        destinationFolder = "TempBuild";
        try
        {
            std::filesystem::remove_all(destinationFolder);
        }
        catch (std::filesystem::filesystem_error error)
        {
            Dialogs::MessageBox message("Temporary build failed", "The temporary folder is currently being used by another process", Dialogs::MessageBox::EMessageType::ERROR, Dialogs::MessageBox::EButtonLayout::OK);
            return;
        }
    }
    else if (auto res = SelectBuildFolder(); res.has_value())
        destinationFolder = res.value();
    else
        return; // Operation cancelled (No folder selected)

    BuildAtLocation(m_context.projectSettings.Get<bool>("dev_build") ? "Development" : "Shipping", destinationFolder, p_autoRun);
}

void Editor::Core::EditorActions::BuildAtLocation(const std::string& p_configuration, const std::string p_buildPath, bool p_autoRun)
{
    std::string buildPath(p_buildPath);
    std::string executableName = m_context.projectSettings.Get<std::string>("executable_name") + ".exe";

    bool failed = false;

    NLS_LOG_INFO("Preparing to build at location: \"" + buildPath + "\"");

    std::filesystem::remove_all(buildPath);

    if (std::filesystem::create_directory(buildPath))
    {
        NLS_LOG_INFO("Build directory created");

        if (std::filesystem::create_directory(buildPath + "Data/"))
        {
            NLS_LOG_INFO("Data directory created");

            if (std::filesystem::create_directory(buildPath + "Data/User/"))
            {
                NLS_LOG_INFO("Data/User directory created");

                std::error_code err;

                std::filesystem::copy(m_context.projectFilePath, buildPath + "Data/User/Game.ini", err);

                if (!err)
                {
                    NLS_LOG_INFO("Data/User/Game.ini file generated");

                    std::filesystem::copy(m_context.projectAssetsPath, buildPath + "Data/User/Assets/", std::filesystem::copy_options::recursive, err);

                    if (!std::filesystem::exists(buildPath + "Data/User/Assets/" + (m_context.projectSettings.Get<std::string>("start_scene"))))
                    {
                        NLS_LOG_ERROR("Failed to find Start Scene at expected path. Verify your Project Setings.");
                        Dialogs::MessageBox message("Build Failure", "An error occured during the building of your game.\nCheck the console for more information", Dialogs::MessageBox::EMessageType::ERROR, Dialogs::MessageBox::EButtonLayout::OK);
                        std::filesystem::remove_all(buildPath);
                        return;
                    }

                    if (!err)
                    {
                        NLS_LOG_INFO("Data/User/Assets/ directory copied");

                        std::filesystem::copy(m_context.engineAssetsPath, buildPath + "Data/Engine/", std::filesystem::copy_options::recursive, err);

                        if (!err)
                        {
                            NLS_LOG_INFO("Data/Engine/ directory copied");
                        }
                        else
                        {
                            NLS_LOG_INFO("Data/Engine/ directory failed to copy");
                            failed = true;
                        }
                    }
                    else
                    {
                        NLS_LOG_ERROR("Data/User/Assets/ directory failed to copy");
                        failed = true;
                    }
                }
                else
                {
                    NLS_LOG_ERROR("Data/User/Game.ini file failed to generate");
                    failed = true;
                }

                std::string builderFolder = "Builder\\" + p_configuration + "\\";

                if (std::filesystem::exists(builderFolder))
                {
                    std::error_code err;

                    std::filesystem::copy(builderFolder, buildPath, err);

                    if (!err)
                    {
                        NLS_LOG_INFO("Builder data (Dlls and executatble) copied");

                        std::filesystem::rename(buildPath + "OvGame.exe", buildPath + executableName, err);

                        if (!err)
                        {
                            NLS_LOG_INFO("Game executable renamed to " + executableName);

                            if (p_autoRun)
                            {
                                std::string exePath = buildPath + executableName;
                                NLS_LOG_INFO("Launching the game at location: \"" + exePath + "\"");
                                if (std::filesystem::exists(exePath))
                                    Platform::SystemCalls::OpenFile(exePath, buildPath);
                                else
                                {
                                    NLS_LOG_INFO("Failed to start the game: Executable not found");
                                    failed = true;
                                }
                            }
                        }
                        else
                        {
                            NLS_LOG_ERROR("Game executable failed to rename");
                            failed = true;
                        }
                    }
                    else
                    {
                        NLS_LOG_ERROR("Builder data (Dlls and executatble) failed to copy");
                        failed = true;
                    }
                }
                else
                {
                    const std::string buildConfiguration = p_configuration == "Development" ? "Debug" : "Release";
                    NLS_LOG_ERROR("Builder folder for \"" + p_configuration + "\" not found. Verify you have compiled Engine source code in '" + buildConfiguration + "' configuration.");
                    failed = true;
                }
            }
        }
    }
    else
    {
        NLS_LOG_ERROR("Build directory failed to create");
        failed = true;
    }

    if (failed)
    {
        std::filesystem::remove_all(buildPath);
        Dialogs::MessageBox message("Build Failure", "An error occured during the building of your game.\nCheck the console for more information", Dialogs::MessageBox::EMessageType::ERROR, Dialogs::MessageBox::EButtonLayout::OK);
    }
}

void Editor::Core::EditorActions::DelayAction(std::function<void()> p_action, uint32_t p_frames)
{
    m_delayedActions.emplace_back(p_frames + 1, p_action);
}

void Editor::Core::EditorActions::ExecuteDelayedActions()
{
    std::for_each(m_delayedActions.begin(), m_delayedActions.end(), [](std::pair<uint32_t, std::function<void()>>& p_element)
                  {
		--p_element.first;

		if (p_element.first == 0)
			p_element.second(); });

    m_delayedActions.erase(std::remove_if(m_delayedActions.begin(), m_delayedActions.end(), [](std::pair<uint32_t, std::function<void()>>& p_element)
                                          { return p_element.first == 0; }),
                           m_delayedActions.end());
}

Editor::Core::Context& Editor::Core::EditorActions::GetContext()
{
    return m_context;
}

Editor::Core::PanelsManager& Editor::Core::EditorActions::GetPanelsManager()
{
    return m_panelsManager;
}

void Editor::Core::EditorActions::SetActorSpawnAtOrigin(bool p_value)
{
    if (p_value)
        m_actorSpawnMode = EActorSpawnMode::ORIGIN;
    else
        m_actorSpawnMode = EActorSpawnMode::FRONT;
}

void Editor::Core::EditorActions::SetActorSpawnMode(EActorSpawnMode p_value)
{
    m_actorSpawnMode = p_value;
}

void Editor::Core::EditorActions::ResetLayout()
{
    DelayAction([this]()
                { m_context.uiManager->ResetLayout(m_context.editorAssetsPath + "/Settings/layout.ini"); });
}

void Editor::Core::EditorActions::SetSceneViewCameraSpeed(int p_speed)
{
    // EDITOR_PANEL(Panels::SceneView, "Scene View").GetCameraController().SetSpeed((float)p_speed);
}

int Editor::Core::EditorActions::GetSceneViewCameraSpeed()
{
    // return (int)EDITOR_PANEL(Panels::SceneView, "Scene View").GetCameraController().GetSpeed();
    return 0;
}

void Editor::Core::EditorActions::SetAssetViewCameraSpeed(int p_speed)
{
    // EDITOR_PANEL(Panels::AssetView, "Asset View").GetCameraController().SetSpeed((float)p_speed);
}

int Editor::Core::EditorActions::GetAssetViewCameraSpeed()
{
    // return (int)EDITOR_PANEL(Panels::AssetView, "Asset View").GetCameraController().GetSpeed();
    return 0;
}

void Editor::Core::EditorActions::ResetSceneViewCameraPosition()
{
    // EDITOR_PANEL(Panels::SceneView, "Scene View").ResetCameraTransform();
}

void Editor::Core::EditorActions::ResetAssetViewCameraPosition()
{
    // EDITOR_PANEL(Panels::AssetView, "Asset View").ResetCameraTransform();
}

Editor::Core::EditorActions::EEditorMode Editor::Core::EditorActions::GetCurrentEditorMode() const
{
    return m_editorMode;
}

void Editor::Core::EditorActions::SetEditorMode(EEditorMode p_newEditorMode)
{
    m_editorMode = p_newEditorMode;
    EditorModeChangedEvent.Invoke(m_editorMode);
}

void Editor::Core::EditorActions::StartPlaying()
{
    if (m_editorMode == EEditorMode::EDIT)
    {
        // 		m_context.scriptInterpreter->RefreshAll();
        // 		EDITOR_PANEL(Panels::Inspector, "Inspector").Refresh();
        //
        // 		if (m_context.scriptInterpreter->IsOk())
        // 		{
        // 			PlayEvent.Invoke();
        // 			m_sceneBackup.Clear();
        // 			tinyxml2::XMLNode* node = m_sceneBackup.NewElement("root");
        // 			m_sceneBackup.InsertFirstChild(node);
        // 			m_context.sceneManager.GetCurrentScene()->OnSerialize(m_sceneBackup, node);
        // 			m_panelsManager.GetPanelAs<Editor::Panels::GameView>("Game View").Focus();
        // 			m_context.sceneManager.GetCurrentScene()->Play();
        // 			SetEditorMode(EEditorMode::PLAY);
        // 		}
    }
    else
    {
        // m_context.audioEngine->Unsuspend();
        SetEditorMode(EEditorMode::PLAY);
    }
}

void Editor::Core::EditorActions::PauseGame()
{
    SetEditorMode(EEditorMode::PAUSE);
    // m_context.audioEngine->Suspend();
}

void Editor::Core::EditorActions::StopPlaying()
{
    // 	if (m_editorMode != EEditorMode::EDIT)
    // 	{
    // 		ImGui::GetIO().DisableMouseUpdate = false;
    // 		m_context.window->SetCursorMode(Windowing::Cursor::ECursorMode::NORMAL);
    // 		SetEditorMode(EEditorMode::EDIT);
    // 		bool loadedFromDisk = m_context.sceneManager.IsCurrentSceneLoadedFromDisk();
    // 		std::string sceneSourcePath = m_context.sceneManager.GetCurrentSceneSourcePath();
    //
    // 		int64_t focusedActorID = -1;
    //
    // 		if (auto targetActor = EDITOR_PANEL(Panels::Inspector, "Inspector").GetTargetActor())
    // 			focusedActorID = targetActor->GetID();
    //
    // 		m_context.sceneManager.LoadSceneFromMemory(m_sceneBackup);
    // 		if (loadedFromDisk)
    // 			m_context.sceneManager.StoreCurrentSceneSourcePath(sceneSourcePath); // To bo able to save or reload the scene whereas the scene is loaded from memory (Supposed to have no path)
    // 		m_sceneBackup.Clear();
    // 		EDITOR_PANEL(Panels::SceneView, "Scene View").Focus();
    // 		if (auto actorInstance = m_context.sceneManager.GetCurrentScene()->FindActorByID(focusedActorID))
    // 			EDITOR_PANEL(Panels::Inspector, "Inspector").FocusActor(*actorInstance);
    // 	}
}

void Editor::Core::EditorActions::NextFrame()
{
    if (m_editorMode == EEditorMode::PLAY || m_editorMode == EEditorMode::PAUSE)
        SetEditorMode(EEditorMode::FRAME_BY_FRAME);
}

Maths::Vector3 Editor::Core::EditorActions::CalculateActorSpawnPoint(float p_distanceToCamera)
{
    // 	auto& sceneView = m_panelsManager.GetPanelAs<Editor::Panels::SceneView>("Scene View");
    //
    // 	if (auto camera = sceneView.GetCamera())
    // 	{
    // 		return camera->GetPosition() + camera->transform->GetWorldForward() * p_distanceToCamera;
    // 	}

    return Maths::Vector3::Zero;
}

Engine::GameObject& Editor::Core::EditorActions::CreateEmptyActor(bool p_focusOnCreation, Engine::GameObject* p_parent, const std::string& p_name)
{
    const auto currentScene = m_context.sceneManager.GetCurrentScene();
    auto& instance = p_name.empty() ? currentScene->CreateGameObject() : currentScene->CreateGameObject(p_name);

    if (p_parent)
        instance.SetParent(*p_parent);

    if (m_actorSpawnMode == EActorSpawnMode::FRONT)
        instance.GetTransform()->SetLocalPosition(CalculateActorSpawnPoint(10.0f));

    if (p_focusOnCreation)
        SelectActor(instance);

    NLS_LOG_INFO("GameObject created");

    return instance;
}

Engine::GameObject& Editor::Core::EditorActions::CreateActorWithModel(const std::string& p_path, bool p_focusOnCreation, Engine::GameObject* p_parent, const std::string& p_name)
{
    auto& instance = CreateEmptyActor(false, p_parent, p_name);

    auto modelRenderer = instance.AddComponent<Engine::Components::MeshRenderer>();

    const auto model = m_context.modelManager[p_path];
    if (model)
        modelRenderer->SetModel(model);

    auto materialRenderer = instance.AddComponent<Engine::Components::MaterialRenderer>();
    const auto material = m_context.materialManager[":Materials\\Default.mat"];
    if (material)
        materialRenderer->FillWithMaterial(*material);

    if (p_focusOnCreation)
        SelectActor(instance);

    return instance;
}

Engine::GameObject& NLS::Editor::Core::EditorActions::CreateActor(const std::string& path, bool focusOnCreation, Engine::GameObject* p_parent)
{
    const auto currentScene = m_context.sceneManager.GetCurrentScene();
    auto* actor = NLS_SERVICE(Engine::ActorManager).CreateResource(path);

    currentScene->AddActor(actor);

    auto& instance = *(actor->GetGameObject());

    if (p_parent)
        instance.SetParent(*p_parent);

    if (m_actorSpawnMode == EActorSpawnMode::FRONT)
        instance.GetTransform()->SetLocalPosition(CalculateActorSpawnPoint(10.0f));

    if (focusOnCreation)
        SelectActor(instance);

    return instance;
}

bool Editor::Core::EditorActions::DestroyActor(Engine::GameObject& p_actor)
{
    p_actor.MarkAsDestroy();
    NLS_LOG_INFO("GameObject destroyed");
    return true;
}

std::string FindDuplicatedActorUniqueName(Engine::GameObject& p_duplicated, Engine::GameObject& p_newActor, Engine::SceneSystem::Scene& p_scene)
{
    const auto parent = p_newActor.GetParent();
    const auto adjacentActors = parent ? parent->GetChildren() : p_scene.GetActors();

    auto availabilityChecker = [&parent, &adjacentActors](std::string target) -> bool
    {
        const auto isActorNameTaken = [&target, parent](auto actor)
        { return (parent || !actor->GetParent()) && actor->GetName() == target; };
        return std::find_if(adjacentActors.begin(), adjacentActors.end(), isActorNameTaken) == adjacentActors.end();
    };

    return Utils::String::GenerateUnique(p_duplicated.GetName(), availabilityChecker);
}

void Editor::Core::EditorActions::DuplicateActor(Engine::GameObject& p_toDuplicate, Engine::GameObject* p_forcedParent, bool p_focus)
{
    // 	tinyxml2::XMLDocument doc;
    // 	tinyxml2::XMLNode* actorsRoot = doc.NewElement("actors");
    // 	p_toDuplicate.OnSerialize(doc, actorsRoot);
    // 	auto& newActor = CreateEmptyActor(false);
    // 	int64_t idToUse = newActor.GetID();
    // 	tinyxml2::XMLElement* currentActor = actorsRoot->FirstChildElement("actor");
    // 	newActor.OnDeserialize(doc, currentActor);
    //
    // 	newActor.SetID(idToUse);
    //
    // 	if (p_forcedParent)
    // 		newActor.SetParent(*p_forcedParent);
    // 	else
    // 	{
    //         auto currentScene = m_context.sceneManager.GetCurrentScene();
    //
    //         if (newActor.GetParentID() > 0)
    //         {
    //             if (auto found = currentScene->FindActorByID(newActor.GetParentID()); found)
    //             {
    //                 newActor.SetParent(*found);
    //             }
    //         }
    //
    //         const auto uniqueName = FindDuplicatedActorUniqueName(p_toDuplicate, newActor, *currentScene);
    //         newActor.SetName(uniqueName);
    // 	}
    //
    // 	if (p_focus)
    // 		SelectActor(newActor);
    //
    // 	for (auto& child : p_toDuplicate.GetChildren())
    // 		DuplicateActor(*child, &newActor, false);
}

void Editor::Core::EditorActions::SelectActor(Engine::GameObject& p_target)
{
    EDITOR_PANEL(Panels::Inspector, "Inspector").FocusActor(p_target);
}

void Editor::Core::EditorActions::UnselectActor()
{
    EDITOR_PANEL(Panels::Inspector, "Inspector").UnFocus();
}

bool Editor::Core::EditorActions::IsAnyActorSelected() const
{
    return EDITOR_PANEL(Panels::Inspector, "Inspector").GetTargetActor();
}

Engine::GameObject* Editor::Core::EditorActions::GetSelectedActor() const
{
    return EDITOR_PANEL(Panels::Inspector, "Inspector").GetTargetActor();
}

void Editor::Core::EditorActions::MoveToTarget(Engine::GameObject& p_target)
{
    EDITOR_PANEL(Panels::SceneView, "Scene View").GetCameraController().MoveToTarget(p_target);
}

void Editor::Core::EditorActions::CompileShaders()
{
    for (auto shader : m_context.shaderManager.GetResources())
        Render::Resources::Loaders::ShaderLoader::Recompile(*shader.second, GetRealPath(shader.second->path));
}

void Editor::Core::EditorActions::SaveMaterials()
{
    for (auto& [id, material] : m_context.materialManager.GetResources())
        Render::Resources::Loaders::MaterialLoader::Save(*material, GetRealPath(material->path));
}

bool Editor::Core::EditorActions::ImportAsset(const std::string& p_initialDestinationDirectory)
{
    using namespace Dialogs;

    std::string modelFormats = "*.fbx;*.obj;";
    std::string textureFormats = "*.png;*.jpeg;*.jpg;*.tga";
    std::string shaderFormats = "*.glsl;";
    std::string soundFormats = "*.mp3;*.ogg;*.wav;";

    OpenFileDialog selectAssetDialog("Select an asset to import", "", {"Any supported format", modelFormats + textureFormats + shaderFormats + soundFormats, "Model (.fbx, .obj)", modelFormats, "Texture (.png, .jpeg, .jpg, .tga)", textureFormats, "Shader (.glsl)", shaderFormats, "Sound (.mp3, .ogg, .wav)", soundFormats});

    if (!selectAssetDialog.Result().empty())
    {
        std::string source = selectAssetDialog.Result()[0];
        std::string extension = '.' + Utils::PathParser::GetExtension(source);
        std::string filename = Utils::PathParser::GetElementName(source);

        SaveFileDialog saveLocationDialog("Where to import?", p_initialDestinationDirectory + filename, {extension, extension});

        if (!saveLocationDialog.Result().empty())
        {
            std::string destination = saveLocationDialog.Result();

            if (!std::filesystem::exists(destination) || MessageBox("File already exists", "The destination you have selected already exists, importing this file will erase the previous file content, are you sure about that?", MessageBox::EMessageType::WARNING, MessageBox::EButtonLayout::OK_CANCEL).GetUserAction() == MessageBox::EUserAction::OK)
            {
                std::filesystem::copy(source, destination, std::filesystem::copy_options::overwrite_existing);
                NLS_LOG_INFO("Asset \"" + destination + "\" imported");
                return true;
            }
        }
    }

    return false;
}

bool Editor::Core::EditorActions::ImportAssetAtLocation(const std::string& p_destination)
{
    using namespace Dialogs;

    std::string modelFormats = "*.fbx;*.obj;";
    std::string textureFormats = "*.png;*.jpeg;*.jpg;*.tga;";
    std::string shaderFormats = "*.glsl;";
    std::string soundFormats = "*.mp3;*.ogg;*.wav;";

    OpenFileDialog selectAssetDialog("Select an asset to import", "", {"Any supported format", modelFormats + textureFormats + shaderFormats + soundFormats, "Model (.fbx, .obj)", modelFormats, "Texture (.png, .jpeg, .jpg, .tga)", textureFormats, "Shader (.glsl)", shaderFormats, "Sound (.mp3, .ogg, .wav)", soundFormats});

    if (!selectAssetDialog.Result().empty())
    {
        std::string source = selectAssetDialog.Result()[0];
        std::string destination = p_destination + Utils::PathParser::GetElementName(source);

        if (!std::filesystem::exists(destination) || MessageBox("File already exists", "The destination you have selected already exists, importing this file will erase the previous file content, are you sure about that?", MessageBox::EMessageType::WARNING, MessageBox::EButtonLayout::OK_CANCEL).GetUserAction() == MessageBox::EUserAction::OK)
        {
            std::filesystem::copy(source, destination, std::filesystem::copy_options::overwrite_existing);
            NLS_LOG_INFO("Asset \"" + destination + "\" imported");
            return true;
        }
    }

    return false;
}

// Duplicate from AResourceManager.h
std::string Editor::Core::EditorActions::GetRealPath(const std::string& p_path)
{
    std::string result;

    if (p_path[0] == ':') // The path is an engine path
    {
        result = m_context.engineAssetsPath + std::string(p_path.data() + 1, p_path.data() + p_path.size());
    }
    else // The path is a project path
    {
        result = m_context.projectAssetsPath + p_path;
    }

    return result;
}

std::string Editor::Core::EditorActions::GetResourcePath(const std::string& p_path, bool p_isFromEngine)
{
    std::string result = p_path;

    if (Utils::String::Replace(result, p_isFromEngine ? m_context.engineAssetsPath : m_context.projectAssetsPath, ""))
    {
        if (p_isFromEngine)
            result = ':' + result;
    }

    return result;
}

void Editor::Core::EditorActions::PropagateFolderRename(std::string p_previousName, std::string p_newName)
{
    p_previousName = Utils::PathParser::MakeNonWindowsStyle(p_previousName);
    p_newName = Utils::PathParser::MakeNonWindowsStyle(p_newName);

    for (auto& p : std::filesystem::recursive_directory_iterator(p_newName))
    {
        if (!p.is_directory())
        {
            std::string newFileName = Utils::PathParser::MakeNonWindowsStyle(p.path().string());
            std::string previousFileName;

            for (char c : newFileName)
            {
                previousFileName += c;
                if (previousFileName == p_newName)
                    previousFileName = p_previousName;
            }

            PropagateFileRename(Utils::PathParser::MakeNonWindowsStyle(previousFileName), Utils::PathParser::MakeNonWindowsStyle(newFileName));
        }
    }
}

void Editor::Core::EditorActions::PropagateFolderDestruction(std::string p_folderPath)
{
    for (auto& p : std::filesystem::recursive_directory_iterator(p_folderPath))
    {
        if (!p.is_directory())
        {
            PropagateFileRename(Utils::PathParser::MakeNonWindowsStyle(p.path().string()), "?");
        }
    }
}

void Editor::Core::EditorActions::PropagateScriptRename(std::string p_previousName, std::string p_newName)
{

}

void Editor::Core::EditorActions::PropagateFileRename(std::string p_previousName, std::string p_newName)
{
     p_previousName = GetResourcePath(p_previousName);
     p_newName = GetResourcePath(p_newName);
 
     if (p_newName != "?")
     {
         /* If not a real rename is asked (Not delete) */
 
         if (NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ModelManager>().MoveResource(p_previousName, p_newName))
         {
             Render::Resources::Model* resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ModelManager>()[p_newName];
             *reinterpret_cast<std::string*>(reinterpret_cast<char*>(resource) + offsetof(Render::Resources::Model, path)) = p_newName;
         }
 
         if (NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>().MoveResource(p_previousName, p_newName))
         {
             Render::Resources::Texture2D* resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>()[p_newName];
             *reinterpret_cast<std::string*>(reinterpret_cast<char*>(resource) + offsetof(Render::Resources::Texture2D, path)) = p_newName;
         }
 
         if (NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ShaderManager>().MoveResource(p_previousName, p_newName))
         {
             Render::Resources::Shader* resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ShaderManager>()[p_newName];
             *reinterpret_cast<std::string*>(reinterpret_cast<char*>(resource) + offsetof(Render::Resources::Shader, path)) = p_newName;
         }
 
         if (NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().MoveResource(p_previousName, p_newName))
         {
             NLS::Render::Resources::Material* resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>()[p_newName];
             *reinterpret_cast<std::string*>(reinterpret_cast<char*>(resource) + offsetof(Render::Resources::Material, path)) = p_newName;
         }
     }
     else
     {
         if (auto texture = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>().GetResource(p_previousName, false))
         {
             for (auto [name, instance] : NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResources())
                 if (instance)
                     for (auto& [name, value] : instance->GetUniformsData())
                         if (value.has_value() && value.type() == typeid(Render::Resources::Texture2D*))
                             if (std::any_cast<Render::Resources::Texture2D*>(value) == texture)
                                 value = static_cast<Render::Resources::Texture2D*>(nullptr);
 
             auto& assetView = EDITOR_PANEL(Panels::AssetView, "Asset View");
             auto assetViewRes = assetView.GetResource();
             if (auto pval = std::get_if<Render::Resources::Texture2D*>(&assetViewRes); pval && *pval)
                 assetView.ClearResource();
 
             NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>().UnloadResource(p_previousName);
         }
 
         if (auto shader = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ShaderManager>().GetResource(p_previousName, false))
         {
             auto& materialEditor = EDITOR_PANEL(Panels::MaterialEditor, "Material Editor");
 
             for (auto [name, instance] : NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResources())
                 if (instance && instance->GetShader() == shader)
                     instance->SetShader(nullptr);
 
             NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ShaderManager>().UnloadResource(p_previousName);
         }
 
         if (auto model = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ModelManager>().GetResource(p_previousName, false))
         {
             auto& assetView = EDITOR_PANEL(Panels::AssetView, "Asset View");
             auto assetViewRes = assetView.GetResource();
             if (auto pval = std::get_if<Render::Resources::Model*>(&assetViewRes); pval && *pval)
                 assetView.ClearResource();
 
             if (auto currentScene = m_context.sceneManager.GetCurrentScene())
                 for (auto actor : currentScene->GetActors())
                     if (auto modelRenderer = actor->GetComponent<Engine::Components::MeshRenderer>(); modelRenderer && modelRenderer->GetModel() == model)
                         modelRenderer->SetModel(nullptr);
 
             NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ModelManager>().UnloadResource(p_previousName);
         }
 
         if (auto material = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResource(p_previousName, false))
         {
             auto& materialEditor = EDITOR_PANEL(Panels::MaterialEditor, "Material Editor");
 
             if (materialEditor.GetTarget() == material)
                 materialEditor.RemoveTarget();
 
             auto& assetView = EDITOR_PANEL(Panels::AssetView, "Asset View");
             auto assetViewRes = assetView.GetResource();
             if (auto pval = std::get_if<NLS::Render::Resources::Material*>(&assetViewRes); pval && *pval)
                 assetView.ClearResource();
 
             if (auto currentScene = m_context.sceneManager.GetCurrentScene())
                 for (auto actor : currentScene->GetActors())
                     if (auto materialRenderer = actor->GetComponent<Engine::Components::MaterialRenderer>(); materialRenderer)
                         materialRenderer->RemoveMaterialByInstance(*material);
 
             NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().UnloadResource(p_previousName);
         }
     }
 
     switch (Utils::PathParser::GetFileType(p_previousName))
     {
         case Utils::PathParser::EFileType::MATERIAL:
             PropagateFileRenameThroughSavedFilesOfType(p_previousName, p_newName, Utils::PathParser::EFileType::SCENE);
             break;
         case Utils::PathParser::EFileType::MODEL:
             PropagateFileRenameThroughSavedFilesOfType(p_previousName, p_newName, Utils::PathParser::EFileType::SCENE);
             break;
         case Utils::PathParser::EFileType::SHADER:
             PropagateFileRenameThroughSavedFilesOfType(p_previousName, p_newName, Utils::PathParser::EFileType::MATERIAL);
             break;
         case Utils::PathParser::EFileType::TEXTURE:
             PropagateFileRenameThroughSavedFilesOfType(p_previousName, p_newName, Utils::PathParser::EFileType::MATERIAL);
             break;
         case Utils::PathParser::EFileType::SOUND:
             PropagateFileRenameThroughSavedFilesOfType(p_previousName, p_newName, Utils::PathParser::EFileType::SCENE);
             break;
     }
 
     EDITOR_PANEL(Panels::Inspector, "Inspector").Refresh();
     EDITOR_PANEL(Panels::MaterialEditor, "Material Editor").Refresh();
}

void Editor::Core::EditorActions::PropagateFileRenameThroughSavedFilesOfType(const std::string& p_previousName, const std::string& p_newName, Utils::PathParser::EFileType p_fileType)
{
    for (auto& entry : std::filesystem::recursive_directory_iterator(m_context.projectAssetsPath))
    {
        if (Utils::PathParser::GetFileType(entry.path().string()) == p_fileType)
        {
            using namespace std;

            {
                ifstream in(entry.path().string().c_str());
                ofstream out("TEMP");
                string wordToReplace(">" + p_previousName + "<");
                string wordToReplaceWith(">" + p_newName + "<");

                string line;
                size_t len = wordToReplace.length();
                while (getline(in, line))
                {
                    if (Utils::String::Replace(line, wordToReplace, wordToReplaceWith))
                        NLS_LOG_INFO("Asset retargeting: \"" + p_previousName + "\" to \"" + p_newName + "\" in \"" + entry.path().string() + "\"");
                    out << line << '\n';
                }

                out.close();
                in.close();
            }

            std::filesystem::copy_file("TEMP", entry.path(), std::filesystem::copy_options::overwrite_existing);
            std::filesystem::remove("TEMP");
        }
    }
}
