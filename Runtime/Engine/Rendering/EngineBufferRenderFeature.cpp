
#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>
#include <Debug/Logger.h>
#include <cstdlib>
#include <cstring>
#include "Rendering/EngineBufferRenderFeature.h"
#include "Rendering/EngineDrawableDescriptor.h"
using namespace NLS;

namespace
{
	bool ShouldLogFrameConstantDiagnostics()
	{
		static const bool enabled = []()
		{
			return NLS::Render::Settings::IsEnvironmentFlagEnabled("NLS_LOG_RENDER_DRAW_PATH");
		}();
		return enabled;
	}
}

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

	if (ShouldLogFrameConstantDiagnostics())
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
	RefreshExplicitFrameBindingSet();
	RefreshExplicitObjectBindingSet();

	if (m_explicitFrameBindingSet != nullptr)
		commandBuffer.BindBindingSet(NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet, m_explicitFrameBindingSet);
	if (m_explicitObjectBindingSet != nullptr)
		commandBuffer.BindBindingSet(NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet, m_explicitObjectBindingSet);
}

void Engine::Rendering::EngineBufferRenderFeature::RefreshExplicitFrameBindingSet()
{
	if (!m_explicitFrameBindingSetDirty)
		return;

	m_explicitFrameBindingSet.reset();

	NLS::Render::Core::ABaseRenderer::ExplicitUniformBufferBindingDesc bindingDesc;
	bindingDesc.set = NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet;
	bindingDesc.registerSpace = NLS::Render::RHI::BindingPointMap::kFrameBindingSpace;
	bindingDesc.binding = 0u;
	bindingDesc.range = sizeof(Maths::Matrix4) + sizeof(Maths::Vector3) + sizeof(float) + sizeof(Maths::Matrix4);
	bindingDesc.entryName = "FrameConstants";
	bindingDesc.layoutDebugName = "EngineFrameBindingLayout";
	bindingDesc.setDebugName = "EngineFrameBindingSet";
	bindingDesc.snapshotDebugName = "EngineFrameConstantsSnapshot";
	bindingDesc.stageMask = NLS::Render::RHI::ShaderStageMask::AllGraphics;
	m_explicitFrameBindingSet = m_renderer.CreateExplicitUniformBufferBindingSet(*m_hlslFrameBuffer, bindingDesc);
	m_explicitFrameBindingSetDirty = false;
}

void Engine::Rendering::EngineBufferRenderFeature::RefreshExplicitObjectBindingSet()
{
	if (!m_explicitObjectBindingSetDirty)
		return;

	m_explicitObjectBindingSet.reset();

	NLS::Render::Core::ABaseRenderer::ExplicitUniformBufferBindingDesc bindingDesc;
	bindingDesc.set = NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet;
	bindingDesc.registerSpace = NLS::Render::RHI::BindingPointMap::kObjectBindingSpace;
	bindingDesc.binding = 0u;
	bindingDesc.range = sizeof(Maths::Matrix4);
	bindingDesc.entryName = "ObjectConstants";
	bindingDesc.layoutDebugName = "EngineObjectBindingLayout";
	bindingDesc.setDebugName = "EngineObjectBindingSet";
	bindingDesc.snapshotDebugName = "EngineObjectConstantsSnapshot";
	bindingDesc.stageMask = NLS::Render::RHI::ShaderStageMask::AllGraphics;
	m_explicitObjectBindingSet = m_renderer.CreateExplicitUniformBufferBindingSet(*m_hlslObjectBuffer, bindingDesc);
	m_explicitObjectBindingSetDirty = false;
}
