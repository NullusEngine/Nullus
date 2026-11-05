#pragma once
#include <Input/InputAction.h>
#include <filesystem>

namespace NLS::Editor::Inputs
{
// Editor/build adapter only. Generated wrappers resolve the host's InputUser;
// they do not create a second input runtime or embed mutable action state.
std::string InputCSharpClassName(const std::filesystem::path& assetPath);
bool GenerateInputCSharp(const Input::InputDefinition& definition, std::string_view className,
    std::string_view nameSpace, std::string& source, std::string& error);
bool WriteInputCSharp(const std::filesystem::path& assetPath, const Input::InputDefinition& definition, std::string& error);
}
