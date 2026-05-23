#pragma once

#include <string>

namespace NLS::Editor::Assets
{
bool IsBuiltInResourcePath(const std::string& resourcePath);
std::string GetBuiltInResourceDisplayName(const std::string& resourcePath);
}
