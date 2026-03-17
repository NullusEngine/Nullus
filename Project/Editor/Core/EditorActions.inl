namespace NLS::Editor::Core
{
	template<typename T>
	inline Engine::GameObject & EditorActions::CreateMonoComponentActor(bool p_focusOnCreation, Engine::GameObject* p_parent)
	{
		auto& instance = CreateEmptyActor(false, p_parent);

		instance.AddComponent<T>();

        instance.SetName(std::string(NLS_TYPEOF(T).GetName()));

		if (p_focusOnCreation)
			SelectActor(instance);

		return instance;
	}
}
