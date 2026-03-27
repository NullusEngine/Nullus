
#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Debug/Logger.h>
#include "Rendering/EngineBufferRenderFeature.h"
#include "Rendering/EngineDrawableDescriptor.h"
using namespace NLS;
Engine::Rendering::EngineBufferRenderFeature::EngineBufferRenderFeature(NLS::Render::Core::CompositeRenderer& p_renderer)
	: ARenderFeature(p_renderer)
{
	m_engineBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
		/* UBO Data Layout */
		sizeof(Maths::Matrix4) +
		sizeof(Maths::Matrix4) +
		sizeof(Maths::Matrix4) +
		sizeof(Maths::Vector3) +
		sizeof(float) +
		sizeof(Maths::Matrix4),
		0, 0,
		NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW
	);

	m_hlslFrameBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
		sizeof(Maths::Matrix4) + sizeof(Maths::Vector3) + sizeof(float) + sizeof(Maths::Matrix4),
		NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kFrameBindingSpace, 0),
		0,
		NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW
	);

	m_hlslObjectBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
		sizeof(Maths::Matrix4),
		NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0),
		0,
		NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW
	);

	m_startTime = std::chrono::high_resolution_clock::now();
}

void Engine::Rendering::EngineBufferRenderFeature::OnBeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
{
	static std::atomic_uint32_t s_loggedFrameConstants{ 0u };
	auto currentTime = std::chrono::high_resolution_clock::now();
	auto elapsedTime = std::chrono::duration_cast<std::chrono::duration<float>>(currentTime - m_startTime);

	size_t offset = sizeof(Maths::Matrix4);
	m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(p_frameDescriptor.camera->GetViewMatrix()), std::ref(offset));
	m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(p_frameDescriptor.camera->GetProjectionMatrix()), std::ref(offset));
	m_engineBuffer->SetSubData(p_frameDescriptor.camera->GetPosition(), std::ref(offset));
	m_engineBuffer->SetSubData(elapsedTime.count(), std::ref(offset));
	m_engineBuffer->Bind(0);

	size_t hlslFrameOffset = 0;
	const auto viewProjection = p_frameDescriptor.camera->GetProjectionMatrix() * p_frameDescriptor.camera->GetViewMatrix();
	auto viewMatrixNoTranslation = p_frameDescriptor.camera->GetViewMatrix();
	viewMatrixNoTranslation(0, 3) = 0.0f;
	viewMatrixNoTranslation(1, 3) = 0.0f;
	viewMatrixNoTranslation(2, 3) = 0.0f;
	const auto viewProjectionNoTranslation = p_frameDescriptor.camera->GetProjectionMatrix() * viewMatrixNoTranslation;

	if (s_loggedFrameConstants.fetch_add(1u) < 4u)
	{
		const auto& cameraPos = p_frameDescriptor.camera->GetPosition();
		const auto& clearColor = p_frameDescriptor.camera->GetClearColor();
		NLS_LOG_INFO(
			"[FrameConstants] renderSize=" +
			std::to_string(p_frameDescriptor.renderWidth) + "x" + std::to_string(p_frameDescriptor.renderHeight) +
			" cameraPos=(" + std::to_string(cameraPos.x) + "," + std::to_string(cameraPos.y) + "," + std::to_string(cameraPos.z) + ")" +
			" clearColor=(" + std::to_string(clearColor.x) + "," + std::to_string(clearColor.y) + "," + std::to_string(clearColor.z) + ")" +
			" vpNoTrans_row0=(" + std::to_string(viewProjectionNoTranslation.data[0]) + "," + std::to_string(viewProjectionNoTranslation.data[1]) + "," + std::to_string(viewProjectionNoTranslation.data[2]) + "," + std::to_string(viewProjectionNoTranslation.data[3]) + ")" +
			" vpNoTrans_row1=(" + std::to_string(viewProjectionNoTranslation.data[4]) + "," + std::to_string(viewProjectionNoTranslation.data[5]) + "," + std::to_string(viewProjectionNoTranslation.data[6]) + "," + std::to_string(viewProjectionNoTranslation.data[7]) + ")");
	}

	m_hlslFrameBuffer->SetSubData(Maths::Matrix4::Transpose(viewProjection), std::ref(hlslFrameOffset));
	m_hlslFrameBuffer->SetSubData(p_frameDescriptor.camera->GetPosition(), std::ref(hlslFrameOffset));
	m_hlslFrameBuffer->SetSubData(elapsedTime.count(), std::ref(hlslFrameOffset));
	m_hlslFrameBuffer->SetSubData(Maths::Matrix4::Transpose(viewProjectionNoTranslation), std::ref(hlslFrameOffset));
	m_hlslFrameBuffer->Bind(NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kFrameBindingSpace, 0));
	m_explicitFrameBindingSetDirty = true;
}

void Engine::Rendering::EngineBufferRenderFeature::OnEndFrame()
{
	m_engineBuffer->Unbind();
	m_hlslFrameBuffer->Unbind();
	m_hlslObjectBuffer->Unbind();
}

void Engine::Rendering::EngineBufferRenderFeature::OnBeforeDraw(NLS::Render::Data::PipelineState& p_pso, const NLS::Render::Entities::Drawable& p_drawable)
{
	EngineDrawableDescriptor descriptor;
	if (p_drawable.TryGetDescriptor<EngineDrawableDescriptor>(descriptor))
	{
		m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(descriptor.modelMatrix), 0);
		m_engineBuffer->SetSubData
		(
			descriptor.userMatrix,

			// UBO layout offset
			sizeof(Maths::Matrix4) +
			sizeof(Maths::Matrix4) +
			sizeof(Maths::Matrix4) +
			sizeof(Maths::Vector3) +
			sizeof(float)
		);

		m_hlslObjectBuffer->SetSubData(Maths::Matrix4::Transpose(descriptor.modelMatrix), 0);
		m_hlslObjectBuffer->Bind(NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0));
		m_explicitObjectBindingSetDirty = true;
	}
}

void Engine::Rendering::EngineBufferRenderFeature::OnPrepareExplicitDraw(
	NLS::Render::RHI::RHICommandBuffer& commandBuffer,
	NLS::Render::Data::PipelineState&,
	const NLS::Render::Entities::Drawable&)
{
	EnsureExplicitBindingLayouts();
	RefreshExplicitFrameBindingSet();
	RefreshExplicitObjectBindingSet();

	if (m_explicitFrameBindingSet != nullptr)
		commandBuffer.BindBindingSet(0u, m_explicitFrameBindingSet);
	if (m_explicitObjectBindingSet != nullptr)
		commandBuffer.BindBindingSet(2u, m_explicitObjectBindingSet);
}

void Engine::Rendering::EngineBufferRenderFeature::EnsureExplicitBindingLayouts()
{
	const auto explicitDevice = m_renderer.GetDriver().GetExplicitDevice();
	if (explicitDevice == nullptr)
		return;

	const auto backend = explicitDevice->GetNativeDeviceInfo().backend;
	if (m_explicitFrameBindingLayout != nullptr &&
		m_explicitObjectBindingLayout != nullptr &&
		m_explicitBindingsBackend == backend)
	{
		return;
	}

	m_explicitFrameBindingLayout.reset();
	m_explicitObjectBindingLayout.reset();
	m_explicitFrameBindingSet.reset();
	m_explicitObjectBindingSet.reset();
	m_explicitBindingsBackend = backend;
	m_explicitFrameBindingSetDirty = true;
	m_explicitObjectBindingSetDirty = true;

	NLS::Render::RHI::RHIBindingLayoutDesc frameLayoutDesc;
	frameLayoutDesc.debugName = "EngineFrameBindingLayout";
	frameLayoutDesc.entries.push_back({
		"FrameConstants",
		NLS::Render::RHI::BindingType::UniformBuffer,
		0u,
		0u,
		1u,
		NLS::Render::RHI::ShaderStageMask::AllGraphics
	});
	m_explicitFrameBindingLayout = explicitDevice->CreateBindingLayout(frameLayoutDesc);

	NLS::Render::RHI::RHIBindingLayoutDesc objectLayoutDesc;
	objectLayoutDesc.debugName = "EngineObjectBindingLayout";
	objectLayoutDesc.entries.push_back({
		"ObjectConstants",
		NLS::Render::RHI::BindingType::UniformBuffer,
		2u,
		0u,
		1u,
		NLS::Render::RHI::ShaderStageMask::AllGraphics
	});
	m_explicitObjectBindingLayout = explicitDevice->CreateBindingLayout(objectLayoutDesc);
}

void Engine::Rendering::EngineBufferRenderFeature::RefreshExplicitFrameBindingSet()
{
	if (!m_explicitFrameBindingSetDirty)
		return;

	const auto explicitDevice = m_renderer.GetDriver().GetExplicitDevice();
	if (explicitDevice == nullptr || m_explicitFrameBindingLayout == nullptr)
		return;

	m_explicitFrameBindingSet.reset();

	NLS::Render::RHI::RHIBindingSetDesc frameSetDesc;
	frameSetDesc.layout = m_explicitFrameBindingLayout;
	frameSetDesc.debugName = "EngineFrameBindingSet";
	const auto frameSnapshot = m_hlslFrameBuffer->CreateExplicitSnapshotBuffer("EngineFrameConstantsSnapshot");
	frameSetDesc.entries.push_back({
		0u,
		NLS::Render::RHI::BindingType::UniformBuffer,
		frameSnapshot != nullptr ? frameSnapshot : m_hlslFrameBuffer->GetExplicitRHIBufferHandle(),
		0u,
		sizeof(Maths::Matrix4) + sizeof(Maths::Vector3) + sizeof(float) + sizeof(Maths::Matrix4),
		nullptr,
		nullptr
	});
	m_explicitFrameBindingSet = explicitDevice->CreateBindingSet(frameSetDesc);
	m_explicitFrameBindingSetDirty = false;
}

void Engine::Rendering::EngineBufferRenderFeature::RefreshExplicitObjectBindingSet()
{
	if (!m_explicitObjectBindingSetDirty)
		return;

	const auto explicitDevice = m_renderer.GetDriver().GetExplicitDevice();
	if (explicitDevice == nullptr || m_explicitObjectBindingLayout == nullptr)
		return;

	m_explicitObjectBindingSet.reset();

	NLS::Render::RHI::RHIBindingSetDesc objectSetDesc;
	objectSetDesc.layout = m_explicitObjectBindingLayout;
	objectSetDesc.debugName = "EngineObjectBindingSet";
	const auto objectSnapshot = m_hlslObjectBuffer->CreateExplicitSnapshotBuffer("EngineObjectConstantsSnapshot");
	objectSetDesc.entries.push_back({
		0u,
		NLS::Render::RHI::BindingType::UniformBuffer,
		objectSnapshot != nullptr ? objectSnapshot : m_hlslObjectBuffer->GetExplicitRHIBufferHandle(),
		0u,
		sizeof(Maths::Matrix4),
		nullptr,
		nullptr
	});
	m_explicitObjectBindingSet = explicitDevice->CreateBindingSet(objectSetDesc);
	m_explicitObjectBindingSetDirty = false;
}
