#pragma once

#include <unordered_map>
#include "PlatformDef.h"
#include <Eventing/Event.h>

#include "Windowing/Context/EDeviceError.h"
#include "Windowing/Settings/DeviceSettings.h"
#include "Windowing/Cursor/ECursorShape.h"
#include "Vector2.h"
struct GLFWwindow;
struct GLFWcursor;

namespace NLS::Context
{
	/**
	* Device 代表窗口上下文。 创建窗口需要创建设备
	*/
	class NLS_PLATFORM_API Device
	{
	public:
		/**
		* 将此事件绑定一个侦听器以接收设备错误
		*/
		static Event<EDeviceError, std::string> ErrorEvent;

		/**
		* 构造函数将负责 GLFW 初始化
		*/
		Device(const Settings::DeviceSettings& p_deviceSettings);

		/**
		* 析构函数将处理 GLFW 销毁
		*/
		~Device();

		/**
		* 返回主显示器的大小（以像素为单位）
		*/
		NLS::Maths::Vector2 GetMonitorSize() const;

		/**
		* 返回与给定形状对应的 GLFWcursor 实例
		* @param p_cursorShape
		*/
		GLFWcursor* GetCursorInstance(Cursor::ECursorShape p_cursorShape) const;

		/**
		* 如果当前启用了垂直同步，则返回 true
		*/
		bool HasVsync() const;

		/**
		* 启用或禁用垂直同步
		* @note 必须在创建并将窗口定义为当前上下文后调用此方法
		* @param p_value (True to enable vsync)
		*/
		void SetVsync(bool p_value);

		/**
		* 使用创建的窗口启用输入和事件管理
		* @note 应该每帧调用
		*/
		void PollEvents() const;

		/**
		* 返回自设备启动以来经过的时间
		*/
		float GetElapsedTime() const;

	private:
		void BindErrorCallback();
		void CreateCursors();
		void DestroyCursors();

	private:
		bool m_vsync = true;
		bool m_isAlive = false;
		std::unordered_map<Cursor::ECursorShape, GLFWcursor*> m_cursors;
	};
}