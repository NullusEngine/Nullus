namespace NLS::Editor::Core
{
	template<typename T>
	inline Engine::GameObject & EditorActions::CreateMonoComponentActor(bool p_focusOnCreation, Engine::GameObject* p_parent)
	{
		auto& instance = CreateEmptyActor(false, p_parent);

		UDRefl::SharedObject component = instance.AddComponent(Type_of<T>);

        instance.SetName(std::string(component.GetType().GetName()));

		if (p_focusOnCreation)
			SelectActor(instance);

		return instance;
	}
}