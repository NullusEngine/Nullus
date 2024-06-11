#pragma once

#include <string>
#include "UDRefl/config.hpp"

#define NLS_ASSERT(condition, message) NLS::Debug::Assertion::Assert(condition, message)

namespace NLS::Debug
{
	/**
	* C++ 断言的封装
	*/
class NLS_BASE_API Assertion
	{
	public:

		/**
		* 禁用构造函数
		*/
		Assertion() = delete;

		/**
		* C++ 断言调用封装
		* @param p_condition
		* @param p_message
		*/
		static void Assert(bool p_condition, const std::string& p_message = "");
	};
}