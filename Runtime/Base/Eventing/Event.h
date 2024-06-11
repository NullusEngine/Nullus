#pragma once

#include <functional>
 #include <stdint.h>
#include "UDRefl/config.hpp"
namespace NLS
{
	/**
	* 监听器的ID（注册回调）。
	* 从事件中删除侦听器需要此值
	*/
	using ListenerID = uint64_t;

	/**
	* 包含一组函数回调的简单事件。
	*/
	template<class... ArgTypes>
	class Event
	{
	public:
		/**
		*	没有返回值的通用函数
		*/
		using Callback = std::function<void(ArgTypes...)>;

		/**
		* 为该事件添加回调函数
		* 同时返回新监听器的ID（如果您想稍后删除监听器，您应该存储返回的ID）
		* @param p_call
		*/
		ListenerID AddListener(Callback p_callback);

		/**
		* 向该事件添加回调函数
		* 同样返回新监听器的ID（如果您想稍后删除监听器，您应该存储返回的ID）
		* @param p_call
		*/
		ListenerID operator+=(Callback p_callback);

		/**
		* 使用侦听器ID删除对此事件的函数回调（调用 AddListener 时创建）
		* @param p_listener
		*/
		bool RemoveListener(ListenerID p_listenerID);

		/**
		* 使用侦听器ID删除对此事件的函数回调（调用 AddListener 时创建）
		* @param p_listener
		*/
		bool operator-=(ListenerID p_listenerID);

		/**
		* 删除此事件的所有侦听器
		*/
		void RemoveAllListeners();

		/**
		* 返回注册的回调数量
		*/
		uint64_t GetListenerCount();

		/**
		* 调用附加到此事件的每个回调
		* @param p_args (Variadic)
		*/
		void Invoke(ArgTypes... p_args);

	private:
		std::unordered_map<ListenerID, Callback>	m_callbacks;
		ListenerID									m_availableListenerID = 0;
	};
}

#include "Event.inl"