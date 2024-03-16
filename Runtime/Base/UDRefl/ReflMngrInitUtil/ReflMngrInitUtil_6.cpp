#include "ReflMngrInitUtil.hpp"

using namespace NLS::UDRefl;

void NLS::UDRefl::details::ReflMngrInitUtil_6(ReflMngr& mngr) {
	mngr.RegisterType<std::string>();
	mngr.AddConstructor<std::string, const std::string_view&>();
	mngr.AddConstructor<std::string, const char* const&>();
	mngr.AddConstructor<std::string, const char* const&, const std::string::size_type&>();
	mngr.AddConstructor<std::string, const std::string::size_type&, const std::string::value_type&>();
}
