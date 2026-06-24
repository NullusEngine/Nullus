#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace NLS::Editor::Assets
{
std::string EncodeModelTextureTextField(std::string_view value);
std::optional<std::string> DecodeModelTextureTextField(std::string_view value);
}
