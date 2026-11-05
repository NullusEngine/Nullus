#pragma once
#include <Platform/PlatformDef.h>
#include <Input/InputUser.h>

namespace NLS::Windowing::Inputs
{
NLS_PLATFORM_API std::string GetControlDisplayName(const Input::ControlPath& path);
NLS_PLATFORM_API std::string GetBindingDisplayName(const Input::InputUser& user, Input::BindingId binding);
}
