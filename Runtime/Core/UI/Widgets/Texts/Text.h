#pragma once

#include "UI/Widgets/AWidget.h"
#include "UI/Widgets/DataWidget.h"

namespace NLS::UI::Widgets::Texts
{
	/**
	* Simple widget to display text on a panel
	*/
	class NLS_CORE_API Text : public DataWidget<std::string>
	{
	public:
		/**
		* Constructor
		* @param p_content
		*/
		Text(const std::string& p_content = "", float p_scale = 1.f);

	protected:
		virtual void _Draw_Impl() override;

	public:
		std::string content;
        float m_scale;
	};
}