#pragma once

#include "Core/EditorActions.h"

namespace NLS::Editor::Core
{
	template<typename T>
	inline Engine::GameObject & EditorActions::CreateMonoComponentActor(bool p_focusOnCreation, Engine::GameObject* p_parent)
	{
		auto& instance = CreateEmptyActor(false, p_parent);

		T component = instance.AddComponent<T>();

        instance.SetName(component->GetName());

		if (p_focusOnCreation)
			SelectActor(instance);

		return instance;
	}
}