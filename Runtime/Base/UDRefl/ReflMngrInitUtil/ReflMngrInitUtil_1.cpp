#include "ReflMngrInitUtil.hpp"

using namespace NLS::UDRefl;

void NLS::UDRefl::details::ReflMngrInitUtil_1(ReflMngr& mngr) {
	mngr.RegisterType<std::int8_t>();
	mngr.RegisterType<std::int16_t>();
	mngr.RegisterType<std::int32_t>();
	mngr.RegisterType<std::int64_t>();
}
