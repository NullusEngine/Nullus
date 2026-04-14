#pragma once

#include <memory>

#include <Vector2.h>

#include "UI/UIDef.h"
#include "UI/Widgets/AWidget.h"

namespace NLS::Render::RHI
{
    class RHITextureView;
}

namespace NLS::UI::Widgets
{
	/**
	* Simple widget that display an image
	*/
	class NLS_UI_API Image : public AWidget
	{
	public:
		/**
		* Constructor
		* @param p_textureView
		* @parma p_size
		*/
		Image(std::shared_ptr<NLS::Render::RHI::RHITextureView> p_textureView, const Maths::Vector2& p_size);

	protected:
		void _Draw_Impl() override;

	public:
		std::shared_ptr<NLS::Render::RHI::RHITextureView> textureView;
		Maths::Vector2 size;
		bool flipVertically = true;

		const Maths::Vector2& GetLastDrawMin() const { return m_lastDrawMin; }
		const Maths::Vector2& GetLastDrawMax() const { return m_lastDrawMax; }
		bool HasLastDrawBounds() const { return m_hasLastDrawBounds; }
		bool WasHoveredLastDraw() const { return m_hoveredLastDraw; }

	private:
		Maths::Vector2 m_lastDrawMin { 0.0f, 0.0f };
		Maths::Vector2 m_lastDrawMax { 0.0f, 0.0f };
		bool m_hasLastDrawBounds = false;
		bool m_hoveredLastDraw = false;
	};
}
