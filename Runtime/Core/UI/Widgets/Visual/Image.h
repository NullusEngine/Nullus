#pragma once

#include <Vector2.h>

#include "UI/Internal/TextureID.h"
#include "UI/Widgets/AWidget.h"
#include "Core/CoreDef.h"
namespace NLS::UI::Widgets::Visual
{
	/**
	* Simple widget that display an image
	*/
	class NLS_CORE_API Image : public AWidget
	{
	public:
		/**
		* Constructor
		* @param p_textureID
		* @parma p_size
		*/
		Image(uint32_t p_textureID, const Maths::Vector2& p_size);

	protected:
		void _Draw_Impl() override;

	public:
		Internal::TextureID textureID;
		Maths::Vector2 size;
	};
}