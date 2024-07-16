#pragma once

#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Features/ARenderFeature.h"
#include "Rendering/Data/FrameInfo.h"
#include "Rendering/Entities/Light.h"
#include "Rendering/Buffers/ShaderStorageBuffer.h"
#include "Rendering/Data/Frustum.h"
#include "RenderDef.h"
namespace NLS::Render::Features
{
	class NLS_RENDER_API LightingRenderFeature : public ARenderFeature
	{
	public:
		// TODO: Consider not using references here, but copying the light instead (should be fairly cheap and doesn't require to keep an instance outside of the scope)
		using LightSet = std::vector<std::reference_wrapper<const NLS::Render::Entities::Light>>;

		struct LightingDescriptor
		{
			LightSet lights;
		};

		/**
		* Constructor
		* @param p_renderer
		* @param p_bufferBindingPoint
		*/
		LightingRenderFeature(NLS::Render::Core::CompositeRenderer& p_renderer, uint32_t p_bufferBindingPoint = 0);


	protected:
		virtual void OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor) override;
		virtual void OnEndFrame() override;

	private:
		uint32_t m_bufferBindingPoint;
		std::unique_ptr<NLS::Render::Buffers::ShaderStorageBuffer> m_lightBuffer;
	};
}
