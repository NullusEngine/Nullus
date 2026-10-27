#pragma once
#include <typeindex>
#include <unordered_map>
#include <UI/Controls/CurveControl.h>
#include <UI/Controls/GradientControl.h>
#include <UI/Controls/SelectionControl.h>
#include <UI/Controls/ResourcePreviewControl.h>
#include <UI/Controls/KeyChordControl.h>

namespace NLS::Editor::Properties
{
struct PropertyControlState
{
    template<class T>
    T& Get()
    {
        auto& value = extensions[std::type_index(typeid(T))];
        if (!value)
            value = std::make_shared<T>();
        return *static_cast<T*>(value.get());
    }
    std::unordered_map<std::type_index, std::shared_ptr<void>> extensions;

    UI::Controls::CurveState curve;
    UI::Controls::GradientState gradient;
    UI::Controls::SelectionState selection;
    UI::Controls::ResourcePreviewState preview;
    UI::Controls::KeyChordState keyChord;
    std::shared_ptr<const void> lifetime = std::make_shared<int>();
    bool mixedReplacement = false;
    uint64_t structureVersion = 0;
};
} // namespace NLS::Editor::Properties
