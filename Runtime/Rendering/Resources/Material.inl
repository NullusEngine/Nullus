#pragma once

#include <Debug/Logger.h>

#include "Rendering/Resources/Material.h"

namespace NLS::Render::Resources
{
	template<typename T>
	inline void Material::Set(const std::string& p_key, const T& p_value)
	{
		if (HasShader())
		{
			if (m_parameterBlock.Contains(p_key) || EnsureMaterialParameterExists(p_key))
			{
				m_parameterBlock.Set(p_key, std::any(p_value));
				InvalidateExplicitBindingSetCache();
			}
		}
		else
		{
			NLS_LOG_ERROR("Material Set failed: No attached shader");
		}
	}

	template<typename T>
	inline const T& Material::Get(const std::string p_key) const
	{
		return std::any_cast<const T&>(m_parameterBlock.Data().at(p_key));
	}
}
