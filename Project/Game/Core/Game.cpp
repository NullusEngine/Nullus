#include "Core/Game.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include <Debug/Logger.h>

#include <Rendering/Context/DriverAccess.h>
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/RHI/Core/RHIResource.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/Settings/EPixelDataFormat.h>
#include <Rendering/Settings/EPixelDataType.h>
#include <Rendering/Tooling/MaterialVisualEvidence.h>

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
		materialValidation.has_value()
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
	}
}

Game::Core::Game::~Game()
{
	m_context.sceneManager.UnloadCurrentScene();
}

void Game::Core::Game::PreUpdate()
{
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

	void RenderCurrentScene(
		Engine::Rendering::BaseSceneRenderer& p_renderer,
		const Game::Context& p_context
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

	std::shared_ptr<Render::RHI::RHITexture> RenderCurrentSceneToFramebuffer(
		Engine::Rendering::BaseSceneRenderer& p_renderer,
		const Game::Context& p_context,
		Render::Buffers::Framebuffer& outputFramebuffer)
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
				p_renderer.EndFrame();
				return outputFramebuffer.GetExplicitTextureHandle();
			}
		}

		return nullptr;
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
	if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_F11))
	{
		if (m_context.inputManager->GetKeyState(Windowing::Inputs::EKey::KEY_LEFT_CONTROL) == Windowing::Inputs::EKeyState::KEY_DOWN)
			Render::Context::DriverUIAccess::OpenLatestRenderDocCapture(*m_context.driver);
		else if (Render::Context::DriverUIAccess::IsRenderDocAvailable(*m_context.driver))
			Render::Context::DriverUIAccess::QueueRenderDocCapture(*m_context.driver, "Game");
	}

	if (auto currentScene = m_context.sceneManager.GetCurrentScene())
	{
		currentScene->Update(p_deltaTime);
		currentScene->LateUpdate(p_deltaTime);
		if (m_materialValidation.has_value() &&
			!m_materialValidationCaptured &&
			m_presentedFrames + 1u >= std::max(1u, m_materialValidation->captureAfterFrames))
		{
			if (m_materialValidationFramebuffer == nullptr)
				m_materialValidationFramebuffer = std::make_unique<Render::Buffers::Framebuffer>();
			m_pendingMaterialValidationReadbackTexture = RenderCurrentSceneToFramebuffer(
				*m_sceneRenderer,
				m_context,
				*m_materialValidationFramebuffer);
		}
		else
		{
			RenderCurrentScene(*m_sceneRenderer, m_context);
		}
	}

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

		const auto readbackTexture = m_pendingMaterialValidationReadbackTexture != nullptr
			? m_pendingMaterialValidationReadbackTexture
			: Render::Context::DriverRendererAccess::ResolveReadbackTexture(*m_context.driver);
		const std::string outputPath = m_materialValidation->outputPath;
		const std::string summaryPath = m_materialValidation->summaryPath.empty()
			? BuildDefaultSummaryPath(outputPath)
			: m_materialValidation->summaryPath;

		if (readbackTexture == nullptr)
		{
			const std::string message = "material validation readback source is unavailable";
			NLS_LOG_ERROR(message);
			WriteMaterialValidationFailureSummary(summaryPath, message);
			m_context.window->SetShouldClose(true);
			m_context.inputManager->ClearEvents();
			return;
		}

		const auto& desc = readbackTexture->GetDesc();
		const uint32_t width = desc.extent.width;
		const uint32_t height = desc.extent.height;
		const uint32_t channels = 4u;
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * channels, 0u);

		const auto readback = Render::Context::DriverRendererAccess::ReadPixelsChecked(
			*m_context.driver,
			readbackTexture,
			0u,
			0u,
			width,
			height,
			Render::Settings::EPixelDataFormat::RGBA,
			Render::Settings::EPixelDataType::UNSIGNED_BYTE,
			pixels.data());

		if (!readback.Succeeded())
		{
			const std::string message = "material validation readback failed: " + readback.message;
			NLS_LOG_ERROR(message);
			WriteMaterialValidationFailureSummary(summaryPath, message);
			m_context.window->SetShouldClose(true);
			m_context.inputManager->ClearEvents();
			return;
		}

		const auto evidence = Render::Tooling::AnalyzeMaterialVisualEvidence(
			pixels.data(),
			width,
			height,
			channels,
			width * channels,
			Render::Tooling::BuildDefaultImportedMaterialVisualProbes());

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
