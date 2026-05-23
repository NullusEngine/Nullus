namespace NLS::Editor::Core
{
	template<typename T>
	inline Engine::GameObject* EditorActions::CreateMonoComponentGameObject(bool p_focusOnCreation, Engine::GameObject* p_parent)
	{
		auto* instance = CreateEmptyGameObject(false, p_parent);
        if (!instance)
            return nullptr;

		instance->AddComponent<T>();

        instance->SetName(std::string(NLS_TYPEOF(T).GetName()));

		if (p_focusOnCreation)
			SelectGameObject(*instance);

		return instance;
	}
}
