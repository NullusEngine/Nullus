#pragma once
namespace NLS::Plugins
{
class PluginManager;
// Defined once by CMake in each executable, never by a DLL's static initializer.
void RegisterCompiledSourcePlugins(PluginManager& manager);
}
