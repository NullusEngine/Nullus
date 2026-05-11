#pragma once

#include <string>

#include <Eventing/Event.h>

#include "UI/Plugins/DragDrop.h"
#include "UI/Plugins/IPlugin.h"

namespace NLS::UI
{
	/**
	* Represents a drag and drop source
	*/
	template<typename T>
	class DDSource : public IPlugin
	{
	public:
		/**
		* Create the drag and drop source
		* @param p_identifier
		* @param p_tooltip
		* @param p_data
		*/
		DDSource
		(
			const std::string& p_identifier,
			const std::string& p_tooltip,
			T p_data
		) : identifier(p_identifier), tooltip(p_tooltip), data(p_data)
		{}

		/**
		* Execute the behaviour of the drag and drop source
		*/
		virtual void Execute() override
		{
			DragDropSourceFlags src_flags = DragDropSourceFlags::NoDisableHover; // Keep the source displayed as hovered
			src_flags |= DragDropSourceFlags::NoHoldToOpenOthers; // Because our dragging is local, we disable the feature of opening foreign treenodes/tabs while dragging

			if (!hasTooltip)
				src_flags |= DragDropSourceFlags::NoPreviewTooltip; // Hide the tooltip

			if (BeginDragDropSource(src_flags))
			{
				if (!m_isDragged)
					DragStartEvent.Invoke();

				m_isDragged = true;

				if (!HasFlag(src_flags, DragDropSourceFlags::NoPreviewTooltip))
					DrawDragDropTooltipText(tooltip.c_str());
				SetDragDropPayload(identifier.c_str(), &data, sizeof(data));
				EndDragDropSource();
			}
			else
			{
				if (m_isDragged)
					DragStopEvent.Invoke();

				m_isDragged = false;
			}
		}

		/**
		* Returns true if the drag and drop source is dragged
		*/
		bool IsDragged() const
		{
			return m_isDragged;
		}

	public:
		std::string identifier;
		std::string tooltip;
		T data;
		Event<> DragStartEvent;
		Event<> DragStopEvent;

		bool hasTooltip = true;

	private:
		bool m_isDragged;
	};
}
