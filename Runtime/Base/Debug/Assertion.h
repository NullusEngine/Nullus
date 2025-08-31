#pragma once

#include <string>
#include "UDRefl/config.hpp"

#define NLS_ASSERT(assertion, ...) (!!(assertion) ||                                 \
 (NLS::Debug::Assert(__FILE__, __FUNCTION__, __LINE__,##__VA_ARGS__), 0)) \

namespace NLS::Debug
{
	/**
	* C++ 断言的封装
	*/
    template<typename... Args>
    void Assert(
        const std::string& file,
        const std::string& function,
        unsigned line,
        const std::string& format,
        const Args&... args
    );
}
 
#include "Assertion.inl"