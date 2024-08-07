#pragma once

#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Features/ARenderFeature.h"
#include "Rendering/Data/FrameInfo.h"
#include "RenderDef.h"
namespace NLS::Render::Features
{
	class NLS_RENDER_API FrameInfoRenderFeature : public NLS::Render::Features::ARenderFeature
	{
	public:
		/**
		* Constructor
		* @param p_renderer
		*/
		FrameInfoRenderFeature(NLS::Render::Core::CompositeRenderer& p_renderer);

		/**
		* Destructor
		*/
		virtual ~FrameInfoRenderFeature();

		/**
		* Return a reference to the last frame info
		* @note Will throw an error if called during the rendering of a frame
		*/
		const NLS::Render::Data::FrameInfo& GetFrameInfo() const;

	protected:
		virtual void OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor) override;
		virtual void OnEndFrame() override;
		virtual void OnAfterDraw(const NLS::Render::Entities::Drawable& p_drawable) override;

	private:
		bool m_isFrameInfoDataValid;
		NLS::Render::Data::FrameInfo m_frameInfo;
		NLS::ListenerID m_postDrawListener;
	};
}
