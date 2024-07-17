#pragma once

#include <string>

#include <Eventing/Event.h>

#include "ImGui/imgui.h"

#include "UI/Plugins/IPlugin.h"
#include "UI/UIManager.h"
#include "ServiceLocator.h"

namespace NLS::UI::Plugins
{
	/**
	* Represents a drag and drop target
	*/
	template<typename T>
	class DDTarget : public IPlugin
	{
	public:
		/**
		* Create the drag and drop target
		* @param p_identifier
		*/
		DDTarget(const std::string& p_identifier) : identifier(p_identifier)
		{}

		/**
		* Execute the drag and drop target behaviour
		* @param p_identifier
		*/
		virtual void Execute() override
		{
            if (NLS_SERVICE(UIManager).BeginDragDropTarget())
			{
				if (!m_isHovered)
					HoverStartEvent.Invoke();

				m_isHovered = true;

				ImGuiDragDropFlags target_flags = 0;
				// target_flags |= ImGuiDragDropFlags_AcceptBeforeDelivery;    // Don't wait until the delivery (release mouse button on a target) to do something
				
				if (!showYellowRect)
					target_flags |= ImGuiDragDropFlags_AcceptNoDrawDefaultRect; // Don't display the yellow rectangle

				if (const ImGuiPayload* payload = NLS_SERVICE(UIManager).AcceptDragDropPayload(identifier.c_str(), target_flags))
				{
					T data = *(T*)payload->Data;
					DataReceivedEvent.Invoke(data);
				}
                NLS_SERVICE(UIManager).EndDragDropTarget();
			}
			else
			{
				if (m_isHovered)
					HoverEndEvent.Invoke();

				m_isHovered = false;
			}
		}

		/**
		* Returns true if the drag and drop target is hovered by a drag and drop source
		*/
		bool IsHovered() const
		{
			return m_isHovered;
		}

	public:
		std::string identifier;
		Event<T> DataReceivedEvent;
		Event<> HoverStartEvent;
		Event<> HoverEndEvent;

		bool showYellowRect = true;

	private:
		bool m_isHovered;
	};
}