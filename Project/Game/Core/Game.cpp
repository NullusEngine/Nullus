#include "Core/Game.h"

#include <Rendering/UI/UIFontAtlas.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include <Debug/Logger.h>

#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Context/RenderingThreadSystem.h>
#include <Jobs/JobSystem.h>
#include <Jobs/BackgroundJobQueue.h>
#include <Rendering/UI/UIDrawSnapshotAdapter.h>
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/RHI/Core/RHIResource.h>
#include <Rendering/RHI/Core/RHISync.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/Settings/EPixelDataFormat.h>
#include <Rendering/Settings/EPixelDataType.h>
#include <Rendering/Tooling/MaterialVisualEvidence.h>
#include <Scripting/ScriptEngineApi.h>
#include <Components/UICanvasComponent.h>
#include <Components/AnimatorComponent.h>
#include <Components/SkinnedMeshRenderer.h>
#include <Components/MeshFilter.h>
#include <Components/MeshRenderer.h>
#include <Core/ResourceManagement/MaterialManager.h>
#include <Rendering/Resources/Mesh.h>
#include <Engine/UI/UIDocumentSourceResolver.h>

#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"

using namespace NLS;

Game::Core::Game::Game(
	Context& p_context,
	std::optional<Launch::MaterialValidationLaunchSettings> materialValidation) :
	m_context(p_context),
	m_sceneRenderer(Engine::Rendering::CreateSceneRenderer(
		*p_context.driver,
			materialValidation.has_value()&&(!materialValidation->animationValidation||materialValidation->animationForward)
			? Engine::Rendering::SceneRendererKind::Forward
			: Engine::Rendering::GetDefaultSceneRendererKind())),
	m_materialValidation(std::move(materialValidation))
{
	if (m_materialValidation.has_value())
	{
		auto diagnostics = Render::Context::DriverRendererAccess::GetDiagnosticsSettings(*m_context.driver);
		diagnostics.logRenderDrawPath = true;
		Render::Context::DriverRendererAccess::SetDiagnosticsSettings(*m_context.driver, diagnostics);
	}

	Assembly::Instance().Instance().Load<AssemblyMath>().Load<AssemblyCore>().Load<AssemblyPlatform>().Load<AssemblyRender>().Load<Engine::AssemblyEngine>();

	const auto startScene = m_context.projectSettings.Get<std::string>("start_scene");
	const auto startScenePath = m_context.projectAssetsPath + startScene;
	if (!startScene.empty() && std::filesystem::exists(startScenePath))
	{
		m_context.sceneManager.LoadScene(startScenePath, true);
	}
	else
	{
		if (!startScene.empty())
			NLS_LOG_WARNING("Start scene not found, loading default validation scene: " + startScenePath);
		m_context.sceneManager.LoadEmptyLightedScene();
	}

	if (auto* scene = m_context.sceneManager.GetCurrentScene())
	{
		scene->Play();
            if(m_materialValidation&&m_materialValidation->animationValidation)
            {
                for(auto* animator:scene->GetFastAccessComponents().animators)
                {animator->Pause();animator->Seek(m_materialValidation->animationTime);}
                scene->UpdateAnimations(0);
            }
	}
}

Game::Core::Game::~Game()
{
	m_context.sceneManager.UnloadCurrentScene();
}

void Game::Core::Game::PreUpdate()
{
	(void)Render::Context::RenderingThreadSystem::PumpGameThreadTasks(64u);
	// Async artifact loads hand their resource uploads back to the game thread;
	// without this drain the JobSystem path records a failure instead of
	// completing, so the pump has to share the frame budget above.
	(void)NLS::Base::Jobs::DrainMainThreadContinuations(64u);
		// Materials requested by skinned scene components must finish even before
		// the first visible drawable exists. Finalization runs on the game thread.
		m_context.materialManager.PumpAsyncLoads(4u);
	m_context.device->PollEvents();
}

	namespace
	{
		std::string BuildDefaultSummaryPath(const std::string& outputPath)
		{
			std::filesystem::path summaryPath(outputPath);
			summaryPath.replace_extension(".txt");
			return summaryPath.string();
		}

		void ResolvePendingUIDocuments(
			Engine::UI::UIRuntimeService& uiRuntimeService,
			const std::string& projectAssetsPath)
		{
			for (auto* canvas : uiRuntimeService.GetCanvases())
			{
				if (canvas == nullptr || canvas->GetDocument() != nullptr || canvas->GetDocumentPath().empty())
					continue;

				// The shared resolver owns the "Assets/..." mapping so the editor,
				// the designer and the game host cannot disagree about it.
				const auto resolvedPath = NLS::Engine::UI::ResolveProjectUIDocumentPath(
					projectAssetsPath,
					canvas->GetDocumentPath());
				std::error_code error;
				if (resolvedPath.empty() || !std::filesystem::is_regular_file(resolvedPath, error) || error)
					continue;

				// Keep the scene's Assets/... reference intact. The resolved source
				// path is development-time data and must never be serialized back.
				(void)canvas->LoadDocumentResolvedPath(resolvedPath, nullptr);
			}
		}

		void RenderCurrentScene(
		Engine::Rendering::BaseSceneRenderer& p_renderer,
			const Game::Context& p_context,
			const Render::Context::PostSubmitTextureReadbackRequest* readback = nullptr
	)
	{
		if (auto currentScene = p_context.sceneManager.GetCurrentScene())
		{
			if (auto camera = currentScene->FindMainCamera())
			{
				auto windowSize = p_context.window->GetSize();

				p_renderer.AddDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
					*currentScene,
					std::nullopt,
					nullptr
				});

				NLS::Render::Data::FrameDescriptor frameDescriptor;
				frameDescriptor.renderWidth = windowSize.x;
				frameDescriptor.renderHeight = windowSize.y;
				frameDescriptor.camera = camera->GetCamera();

				p_renderer.BeginFrame(frameDescriptor);
				p_renderer.DrawFrame();
					if (readback != nullptr)
						p_renderer.SetNextFramePostSubmitTextureReadback(*readback);
				p_renderer.EndFrame();
			}
			else
			{
				NLS_LOG_ERROR("RenderCurrentScene: no camera found");
			}
		}
		else
		{
			NLS_LOG_ERROR("RenderCurrentScene: no current scene");
		}
	}

		void RenderCurrentSceneToFramebuffer(
		Engine::Rendering::BaseSceneRenderer& p_renderer,
		const Game::Context& p_context,
			Render::Buffers::Framebuffer& outputFramebuffer,
			Render::Context::PostSubmitTextureReadbackRequest readback)
	{
		if (auto currentScene = p_context.sceneManager.GetCurrentScene())
		{
			if (auto camera = currentScene->FindMainCamera())
			{
				auto windowSize = p_context.window->GetSize();
				outputFramebuffer.Resize(
					static_cast<uint16_t>(windowSize.x),
					static_cast<uint16_t>(windowSize.y));

				p_renderer.AddDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
					*currentScene,
					std::nullopt,
					nullptr
				});

				NLS::Render::Data::FrameDescriptor frameDescriptor;
				frameDescriptor.renderWidth = static_cast<uint16_t>(windowSize.x);
				frameDescriptor.renderHeight = static_cast<uint16_t>(windowSize.y);
				frameDescriptor.camera = camera->GetCamera();
				NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputFramebuffer);

				p_renderer.BeginFrame(frameDescriptor);
				p_renderer.DrawFrame();
					readback.texture = outputFramebuffer.GetExplicitTextureHandle();
					p_renderer.SetNextFramePostSubmitTextureReadback(std::move(readback));
				p_renderer.EndFrame();
			}
		}

	}

	void WriteMaterialValidationFailureSummary(
		const std::string& summaryPath,
		const std::string& message)
	{
		if (summaryPath.empty())
			return;

		std::error_code error;
		const std::filesystem::path path(summaryPath);
		if (!path.parent_path().empty())
			std::filesystem::create_directories(path.parent_path(), error);

		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (output)
		{
			output << "Material Visual Evidence: FAIL\n";
			output << "Diagnostic: " << message << "\n";
		}
	}
}

void Game::Core::Game::Update(float p_deltaTime)
{
	const float unscaledDelta = std::max(0.0f, p_deltaTime);
	NLS::Scripting::ScriptEngineApi::SetInputManager(m_context.inputManager.get());
	NLS::Scripting::ScriptEngineApi::SetSceneManager(&m_context.sceneManager);
	const float delta = NLS::Scripting::ScriptEngineApi::ScaleDeltaTime(unscaledDelta);
	NLS::Scripting::ScriptEngineApi::AdvanceFrame(unscaledDelta);
	m_unscaledTime += unscaledDelta;
	m_time += delta;
	++m_frameIndex;
	if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_F11))
	{
		if (m_context.inputManager->GetKeyState(Windowing::Inputs::EKey::KEY_LEFT_CONTROL) == Windowing::Inputs::EKeyState::KEY_DOWN)
			Render::Context::DriverUIAccess::OpenLatestRenderDocCapture(*m_context.driver);
		else if (Render::Context::DriverUIAccess::IsRenderDocAvailable(*m_context.driver))
			Render::Context::DriverUIAccess::QueueRenderDocCapture(*m_context.driver, "Game");
	}

	if (auto currentScene = m_context.sceneManager.GetCurrentScene())
	{
		NLS::Scripting::ScriptEngineApi::SetScene(currentScene);
		#if defined(NLS_ENABLE_GAME_UI)
		m_uiRuntimeService.SyncCanvases(currentScene->GetFastAccessComponents().uiCanvases);
		ResolvePendingUIDocuments(m_uiRuntimeService, m_context.projectAssetsPath);
		const auto windowSize = m_context.window->GetSize();
		const Engine::UI::UIVector2 uiViewport {
			std::max(0.0f, windowSize.x),
			std::max(0.0f, windowSize.y) };
		const auto mousePosition = m_context.inputManager->GetMousePosition();
		for (auto* canvas : m_uiRuntimeService.GetCanvases())
		{
			if (canvas && canvas->IsRenderable())
				// Establish layout before hit testing. The normal frame tick below
				// reuses the same tree after scene/script updates.
				canvas->Tick(0.0f, uiViewport, 1.0f, m_frameIndex);
		}
		Engine::UI::InputEventQueue inputEvents;
		inputEvents.Push({
			Engine::UI::UIEventType::PointerMove,
			Engine::UI::UIInputDevice::Mouse,
			m_frameIndex,
			0u,
			0u,
			0u,
			0u,
			{ mousePosition.x, mousePosition.y },
			{},
			0.0f,
			{},
			false });
		if (m_context.inputManager->IsMouseButtonPressed(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT))
		{
			inputEvents.Push({
				Engine::UI::UIEventType::PointerDown,
				Engine::UI::UIInputDevice::Mouse,
				m_frameIndex,
				0u,
				0u,
				0u,
				0u,
				{ mousePosition.x, mousePosition.y },
				{},
				0.0f,
				{},
				false });
		}
		if (m_context.inputManager->IsMouseButtonReleased(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT))
		{
			inputEvents.Push({
				Engine::UI::UIEventType::PointerUp,
				Engine::UI::UIInputDevice::Mouse,
				m_frameIndex,
				0u,
				0u,
				0u,
				0u,
				{ mousePosition.x, mousePosition.y },
				{},
				0.0f,
				{},
				false });
		}
		std::vector<std::pair<uint32_t, Windowing::Inputs::EKeyState>> keyEvents;
		keyEvents.reserve(m_context.inputManager->GetKeyEvents().size());
		for (const auto& [key, state] : m_context.inputManager->GetKeyEvents())
			keyEvents.emplace_back(static_cast<uint32_t>(key), state);
		std::sort(keyEvents.begin(), keyEvents.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.first < rhs.first;
		});
		for (const auto& [keyCode, state] : keyEvents)
		{
			inputEvents.Push({
				state == Windowing::Inputs::EKeyState::KEY_DOWN
					? Engine::UI::UIEventType::KeyDown
					: Engine::UI::UIEventType::KeyUp,
				Engine::UI::UIInputDevice::Keyboard,
				m_frameIndex,
				0u,
				0u,
				keyCode,
				0u,
				{},
				{},
				0.0f,
				{},
				false });
		}
		const auto wheel = m_context.inputManager->GetWheelMovement();
		if (wheel.y != 0.0f)
		{
			inputEvents.Push({
				Engine::UI::UIEventType::Scroll,
				Engine::UI::UIInputDevice::Mouse,
				m_frameIndex,
				0u,
				0u,
				0u,
				0u,
				{ mousePosition.x, mousePosition.y },
				{},
				wheel.y,
				{},
				false });
		}
		m_uiRuntimeService.DispatchInputQueue(inputEvents);
		NLS::Scripting::ScriptFrameContext scriptFrame;
		scriptFrame.deltaTime = delta;
		scriptFrame.unscaledDeltaTime = unscaledDelta;
		scriptFrame.time = m_time;
		scriptFrame.unscaledTime = m_unscaledTime;
		scriptFrame.frameIndex = m_frameIndex;
		scriptFrame.fixedDeltaTime = NLS::Scripting::ScriptEngineApi::GetFixedDeltaTime();
		scriptFrame.timeScale = NLS::Scripting::ScriptEngineApi::GetTimeScale();
		m_fixedAccumulator += delta;
		while (m_fixedAccumulator + 1e-6f >= scriptFrame.fixedDeltaTime)
		{
			NLS::Scripting::ScriptFrameContext fixedFrame = scriptFrame;
			fixedFrame.deltaTime = scriptFrame.fixedDeltaTime * scriptFrame.timeScale;
			fixedFrame.unscaledDeltaTime = scriptFrame.fixedDeltaTime;
			fixedFrame.fixedFrameIndex = ++m_fixedFrameIndex;
			const auto fixedSchedule = m_context.scriptRuntime.BeginScheduledFrame(
				NLS::Scripting::ScriptCallback::FixedUpdate,
				fixedFrame);
			currentScene->FixedUpdate(fixedFrame.deltaTime);
			if (fixedSchedule.Succeeded())
			{
				const auto flushStatus = m_context.scriptRuntime.FlushScheduledFrame();
				if (!flushStatus.Succeeded())
					NLS_LOG_WARNING("Script FixedUpdate batch failed: " + flushStatus.message);
			}
			m_fixedAccumulator -= scriptFrame.fixedDeltaTime;
		}
		const auto updateSchedule = m_context.scriptRuntime.BeginScheduledFrame(
			NLS::Scripting::ScriptCallback::Update,
			scriptFrame);
			currentScene->UpdateGameplay(delta);
		if (updateSchedule.Succeeded())
		{
			const auto flushStatus = m_context.scriptRuntime.FlushScheduledFrame();
			if (!flushStatus.Succeeded())
				NLS_LOG_WARNING("Script Update batch failed: " + flushStatus.message);
		}
			currentScene->UpdateAnimations(delta);
			if(m_materialValidation.has_value()&&(m_frameIndex==1||m_presentedFrames+1==m_materialValidation->captureAfterFrames))
			{
				for(auto* object:currentScene->GetGameObjects())
				{
					if(auto* animator=object->GetComponent<Engine::Components::AnimatorComponent>())
						NLS_LOG_INFO("[AnimationValidation] Animator "+object->GetName()+" pose="+std::to_string(bool(animator->CaptureFrame()))+" error="+animator->LastError());
					if(auto* skinned=object->GetComponent<Engine::Components::SkinnedMeshRenderer>())
					{
						auto* filter=object->GetComponent<Engine::Components::MeshFilter>();
						NLS_LOG_INFO("[AnimationValidation] Skin "+object->GetName()+" mesh="+std::to_string(filter&&filter->ResolveMesh())+" error="+skinned->LastError());
						if(auto* renderer=object->GetComponent<Engine::Components::MeshRenderer>())
							for(const auto& path:renderer->GetMaterialPaths())
								NLS_LOG_INFO("[AnimationValidation] Material path="+path+" resolved="+std::to_string(bool(renderer->ResolveMaterialAtIndex(0)))+" state="+std::to_string(int(m_context.materialManager.TryProbeAsyncArtifactLoad(path))));
						if(auto* geometry=filter?filter->ResolveMesh():nullptr)
							NLS_LOG_INFO("[AnimationValidation] Bounds center="+std::to_string(geometry->GetBounds().center.x)+","+std::to_string(geometry->GetBounds().center.y)+","+std::to_string(geometry->GetBounds().center.z));
					}
				}
			}

		const auto lateSchedule = m_context.scriptRuntime.BeginScheduledFrame(
			NLS::Scripting::ScriptCallback::LateUpdate,
			scriptFrame);
		currentScene->LateUpdate(delta);
		if (lateSchedule.Succeeded())
		{
			const auto flushStatus = m_context.scriptRuntime.FlushScheduledFrame();
			if (!flushStatus.Succeeded())
				NLS_LOG_WARNING("Script LateUpdate batch failed: " + flushStatus.message);
		}
		m_uiRuntimeService.Tick(delta, uiViewport, 1.0f, m_frameIndex);
		// Publish the glyph atlas before the snapshot so text quads have pixels to
		// sample; the upload itself completes on the render thread.
		(void)m_uiFontAtlasUploader.Pump(
			*m_context.driver,
			NLS::Render::UI::GetSharedUIFontAtlas());
		m_uiSnapshot = m_uiRuntimeService.GetLatestSnapshot();
		// Publish only the immutable render snapshot. The RHI overlay keeps
		// ownership until the corresponding frame has retired, so neither the
		// UI tree nor script-owned elements cross the render-thread boundary.
		Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(
			*m_context.driver,
			Render::UI::AdaptUIDrawSnapshot(m_uiSnapshot));
		#else
		m_uiSnapshot.reset();
		#endif
		if (m_materialValidation.has_value() &&
			!m_materialValidationCaptured &&
			m_presentedFrames + 1u >= std::max(1u, m_materialValidation->captureAfterFrames))
		{
				const auto windowSize = m_context.window->GetSize();
				m_materialValidationReadbackWidth = static_cast<uint32_t>(windowSize.x);
				m_materialValidationReadbackHeight = static_cast<uint32_t>(windowSize.y);
				m_materialValidationReadbackPixels = std::make_shared<std::vector<uint8_t>>(
					static_cast<size_t>(m_materialValidationReadbackWidth) * m_materialValidationReadbackHeight * 4u, 0u);
				m_materialValidationReadbackState = std::make_shared<Render::Context::PostSubmitTextureReadbackState>();
				Render::Context::PostSubmitTextureReadbackRequest readback;
				readback.width = m_materialValidationReadbackWidth;
				readback.height = m_materialValidationReadbackHeight;
				readback.destination = m_materialValidationReadbackPixels->data();
				readback.destinationKeepAlive = m_materialValidationReadbackPixels;
				readback.state = m_materialValidationReadbackState;
				if (m_materialValidation->captureSwapchain)
				{
					readback.source = Render::Context::TextureReadbackSource::FrameSwapchain;
					RenderCurrentScene(*m_sceneRenderer, m_context, &readback);
				}
				else
				{
					if (m_materialValidationFramebuffer == nullptr)
						m_materialValidationFramebuffer = std::make_unique<Render::Buffers::Framebuffer>();
					RenderCurrentSceneToFramebuffer(*m_sceneRenderer, m_context,
						*m_materialValidationFramebuffer, std::move(readback));
				}
		}
		else
		{
			RenderCurrentScene(*m_sceneRenderer, m_context);
		}
		}
	#if defined(NLS_ENABLE_GAME_UI)
		else
		{
			m_uiSnapshot.reset();
			Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(
				*m_context.driver,
				nullptr);
		}
	#endif

		NLS::Scripting::ScriptEngineApi::FlushDeferredDestructions();
	if (auto currentScene = m_context.sceneManager.GetCurrentScene())
		currentScene->CollectGarbages();
	m_context.sceneManager.Update();

#ifdef _DEBUG
	if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_R))
		NLS::Render::Resources::Loaders::ShaderLoader::Recompile(
			*m_context.shaderManager[":Shaders/Standard.hlsl"],
			"Data/Engine/Shaders/Standard.hlsl",
			m_context.projectAssetsPath);
#endif
}

void Game::Core::Game::PostUpdate()
{
	Render::Context::DriverUIAccess::PresentSwapchain(*m_context.driver);
	++m_presentedFrames;

	if (m_materialValidation.has_value() &&
		!m_materialValidationCaptured &&
		m_presentedFrames >= std::max(1u, m_materialValidation->captureAfterFrames))
	{
		m_materialValidationCaptured = true;
		Render::Context::DriverRendererAccess::DrainThreadedRendering(*m_context.driver);

		const std::string outputPath = m_materialValidation->outputPath;
		const std::string summaryPath = m_materialValidation->summaryPath.empty()
			? BuildDefaultSummaryPath(outputPath)
			: m_materialValidation->summaryPath;

			Render::RHI::RHIReadbackResult readback {
				Render::RHI::RHIReadbackStatusCode::BackendFailure,
				"material validation readback was not carried into a rendered frame"
			};
			if (m_materialValidationReadbackState != nullptr)
		{
				std::lock_guard lock(m_materialValidationReadbackState->mutex);
				if (m_materialValidationReadbackState->beginAttempted)
				{
					readback.code = m_materialValidationReadbackState->resultCode;
					readback.message = m_materialValidationReadbackState->resultMessage;
					readback.completion = m_materialValidationReadbackState->completion;
				}
		}
			if (readback.Succeeded() && readback.completion != nullptr)
			{
				const auto completion = readback.completion->Wait(300'000'000'000ull);
				if (!completion.Succeeded())
				{
					readback.code = Render::RHI::RHIReadbackStatusCode::BackendFailure;
					readback.message = completion.message.empty()
						? "material validation GPU readback did not complete" : completion.message;
				}
			}

		if (!readback.Succeeded())
		{
			const std::string message = "material validation readback failed: " + readback.message;
			NLS_LOG_ERROR(message);
			WriteMaterialValidationFailureSummary(summaryPath, message);
			m_context.window->SetShouldClose(true);
			m_context.inputManager->ClearEvents();
			return;
		}

			const uint32_t width = m_materialValidationReadbackWidth;
			const uint32_t height = m_materialValidationReadbackHeight;
			const uint32_t channels = 4u;
			const auto& pixels = *m_materialValidationReadbackPixels;
            const std::vector<Render::Tooling::MaterialVisualProbe> animationProbes={
                {"Cooked animation silhouette",{.25f,.1f,.75f,.9f},Render::Tooling::MaterialVisualChannel::Blue}};
			const auto evidence = Render::Tooling::AnalyzeMaterialVisualEvidence(
			pixels.data(),
			width,
			height,
			channels,
			width * channels,
				m_materialValidation->animationValidation?animationProbes:Render::Tooling::BuildDefaultImportedMaterialVisualProbes());

		std::string writeError;
		if (!outputPath.empty() && !Render::Tooling::WriteMaterialVisualEvidencePng(
			outputPath,
			pixels.data(),
			width,
			height,
			channels,
			width * channels,
			&writeError))
		{
			NLS_LOG_ERROR("material validation PNG write failed: " + writeError);
		}

		if (!summaryPath.empty() && !Render::Tooling::WriteMaterialVisualEvidenceReport(
			summaryPath,
			evidence,
			&writeError))
		{
			NLS_LOG_ERROR("material validation summary write failed: " + writeError);
		}

		NLS_LOG_INFO(Render::Tooling::BuildMaterialVisualEvidenceReport(evidence));
		m_context.window->SetShouldClose(true);
	}

	m_context.inputManager->ClearEvents();
}
