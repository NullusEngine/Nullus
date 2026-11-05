#pragma once
#include <memory>
namespace NLS::Core::Modules { class IModule; }
namespace NLS::Examples { std::unique_ptr<Core::Modules::IModule> CreateEditorTools(); }
