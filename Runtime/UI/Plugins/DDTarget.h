#pragma once

#include <string>

#include <Eventing/Event.h>

#include "UI/Plugins/IPlugin.h"
#include "UI/Plugins/DragDrop.h"

namespace NLS::UI
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
			if (BeginDragDropTarget())
			{
				if (!m_isHovered)
					HoverStartEvent.Invoke();

				m_isHovered = true;

				DragDropTargetFlags target_flags = DragDropTargetFlags::None;
				if (acceptBeforeDelivery)
					target_flags |= DragDropTargetFlags::AcceptBeforeDelivery;
				
				if (!showYellowRect)
					target_flags |= DragDropTargetFlags::AcceptNoDrawDefaultRect; // Don't display the yellow rectangle

				if (const DragDropPayloadView payload = AcceptDragDropPayload(identifier.c_str(), target_flags); payload.data != nullptr)
				{
					T data = *static_cast<const T*>(payload.data);
					if (payload.delivered)
						DataReceivedEvent.Invoke(data);
					else
						PreviewReceivedEvent.Invoke(data);
				}
				EndDragDropTarget();
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
		Event<T> PreviewReceivedEvent;
		Event<> HoverStartEvent;
		Event<> HoverEndEvent;

		bool showYellowRect = true;
		bool acceptBeforeDelivery = false;

	private:
		bool m_isHovered = false;
	};
}
