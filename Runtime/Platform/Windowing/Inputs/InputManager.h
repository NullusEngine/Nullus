#pragma once

#include <unordered_map>

#include "Windowing/Window.h"
#include "Windowing/Inputs/EKey.h"
#include "Windowing/Inputs/EKeyState.h"
#include "Windowing/Inputs/EMouseButton.h"
#include "Windowing/Inputs/EMouseButtonState.h"
#include "Singleton.h"
namespace NLS::Windowing::Inputs
{
	/**
	* Handles inputs (Mouse and keyboard)
	*/
	class NLS_PLATFORM_API InputManager
	{
	public:
		static InputManager* Instance;
		/**
		* Create the window
		* @param p_windowSettings
		*/
		InputManager(Window& p_window);

		/**
		* Destroy the input manager by removing listeners on the window
		*/
		~InputManager();

		void Update();
		/**
		* Return the current state of the given key
		* @param p_key
		*/
		EKeyState GetKeyState(EKey p_key) const;

		/**
		* Return the current state of the given mouse button
		* @param p_button
		*/
		EMouseButtonState GetMouseButtonState(EMouseButton p_button) const;

		/**
		* Return true if the given key has been pressed during the frame
		* @param p_key
		*/
		bool IsKeyPressed(EKey p_key) const;

		/**
		* Return true if the given key has been released during the frame
		* @param p_key
		*/
		bool IsKeyReleased(EKey p_key) const;

		/**
		* Return true if the given mouse button has been pressed during the frame
		* @param p_button
		*/
		bool IsMouseButtonPressed(EMouseButton p_button) const;

		/**
		* Return true if the given mouse button has been released during the frame
		* @param p_button
		*/
		bool IsMouseButtonReleased(EMouseButton p_button) const;

		/**
		* Return the current mouse position relative to the window
		*/
		Maths::Vector2 GetMousePosition() const;

		inline Maths::Vector2 GetWheelMovement() const { return lastWheel; }
		/**
		* Clear any event occured
		* @note Should be called at the end of every game tick
		*/
		void ClearEvents();

	private:
		void OnKeyPressed(int p_key);
		void OnKeyReleased(int p_key);
		void OnMouseButtonPressed(int p_button);
		void OnMouseButtonReleased(int p_button);
		void OnMouseScroll(double x, double y);
	private:
		Window& m_window;

		ListenerID m_keyPressedListener;
        ListenerID m_keyReleasedListener;
        ListenerID m_mouseButtonPressedListener;
        ListenerID m_mouseButtonReleasedListener;
		ListenerID m_mouseScrollListener;
		std::unordered_map<EKey, EKeyState>					m_keyEvents;
		std::unordered_map<EMouseButton, EMouseButtonState>	m_mouseButtonEvents;
		Maths::Vector2 lastWheel;
	};
}