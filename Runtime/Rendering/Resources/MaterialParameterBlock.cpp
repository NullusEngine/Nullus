#include "Rendering/Resources/MaterialParameterBlock.h"

namespace NLS::Render::Resources
{
	void MaterialParameterBlock::Clear()
	{
		m_values.clear();
	}

	bool MaterialParameterBlock::Contains(const std::string& name) const
	{
		return m_values.find(name) != m_values.end();
	}

	void MaterialParameterBlock::Set(const std::string& name, std::any value)
	{
		m_values[name] = std::move(value);
	}

	const std::any* MaterialParameterBlock::TryGet(const std::string& name) const
	{
		const auto found = m_values.find(name);
		return found != m_values.end() ? &found->second : nullptr;
	}

	std::any* MaterialParameterBlock::TryGet(const std::string& name)
	{
		const auto found = m_values.find(name);
		return found != m_values.end() ? &found->second : nullptr;
	}

	std::map<std::string, std::any>& MaterialParameterBlock::Data()
	{
		return m_values;
	}

	const std::map<std::string, std::any>& MaterialParameterBlock::Data() const
	{
		return m_values;
	}
}
