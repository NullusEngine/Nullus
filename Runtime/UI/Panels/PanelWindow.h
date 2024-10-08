﻿#pragma once

#include <memory>

#include <Eventing/Event.h>

#include "UI/Panels/APanelTransformable.h"
#include "UI/Settings/PanelWindowSettings.h"

namespace NLS::UI
{
	/**
	* A PanelWindow is a panel that is localized and behave like a window (Movable, resizable...)
	*/
	class NLS_UI_API PanelWindow : public APanelTransformable
	{
	public:
		/**
		* Creates the PanelWindow
		* @param p_name
		* @param p_opened
		* @param p_panelSettings
		*/
		PanelWindow(
			const std::string& p_name = "",
			bool p_opened = true,
			const PanelWindowSettings& p_panelSettings = PanelWindowSettings{}
		);
		virtual ~PanelWindow() noexcept = default;
		/**
		* Open (show) the panel
		*/
		void Open();

		/**
		* Close (hide) the panel
		*/
		void Close();

		/**
		* Focus the panel
		*/
		void Focus();

		/**
		* Defines the opened state of the window
		* @param p_value
		*/
		void SetOpened(bool p_value);

		/**
		* Returns true if the panel is opened
		*/
		bool IsOpened() const;

		/**
		* Returns true if the panel is hovered
		*/
		bool IsHovered() const;

		/**
		* Returns true if the panel is focused
		*/
		bool IsFocused() const;

		/**
		* Returns true if the panel is appearing
		*/
		bool IsAppearing() const;

        /**
        * Scrolls to the bottom of the window
        */
        void ScrollToBottom();

        /**
        * Scrolls to the top of the window
        */
        void ScrollToTop();

        /**
        * Returns true if the window is scrolled to the bottom
        */
        bool IsScrolledToBottom() const;

        /**
        * Returns true if the window is scrolled to the bottom
        */
        bool IsScrolledToTop() const;

	protected:
		void _Draw_Impl() override;

	public:
		std::string name;

		Maths::Vector2 minSize = { 0.f, 0.f };
		Maths::Vector2 maxSize = { 0.f, 0.f };

		PanelWindowSettings panelSettings;

		NLS::Event<> OpenEvent;
		NLS::Event<> CloseEvent;

	private:
		bool m_opened = false;
		bool m_hovered = false;
		bool m_focused = false;
        bool m_mustScrollToBottom = false;
        bool m_mustScrollToTop = false;
        bool m_scrolledToBottom = false;
        bool m_scrolledToTop = false;
	};
}