#pragma once

#include "UI/Widgets/AWidget.h"

namespace NLS::UI::Widgets::Visual
{
	/**
	* Simple widget that display a bullet point
	*/
	class Bullet : public AWidget
	{
	protected:
		virtual void _Draw_Impl() override;
	};
}