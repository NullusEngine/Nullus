#pragma once

#include <any>
#include <map>
#include <string>

#include "RenderDef.h"

namespace NLS::Render::Resources
{
	class NLS_RENDER_API MaterialParameterBlock
	{
	public:
		void Clear();
		bool Contains(const std::string& name) const;
		void Set(const std::string& name, std::any value);
		const std::any* TryGet(const std::string& name) const;
		std::any* TryGet(const std::string& name);
		std::map<std::string, std::any>& Data();
		const std::map<std::string, std::any>& Data() const;

	private:
		std::map<std::string, std::any> m_values;
	};
}
