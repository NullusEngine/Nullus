#include "Rendering/ShaderCompiler/ShaderVariantKey.h"

#include <sstream>

namespace NLS::Render::ShaderCompiler
{
	bool ShaderVariantKey::operator==(const ShaderVariantKey& other) const
	{
		if (assetPath != other.assetPath || stage != other.stage || macros.size() != other.macros.size())
			return false;

		for (size_t index = 0; index < macros.size(); ++index)
		{
			if (macros[index].name != other.macros[index].name || macros[index].value != other.macros[index].value)
				return false;
		}

		return true;
	}

	bool ShaderVariantKey::operator!=(const ShaderVariantKey& other) const
	{
		return !(*this == other);
	}

	std::string ShaderVariantKey::ToString() const
	{
		std::ostringstream stream;
		stream << assetPath << "|" << static_cast<int>(stage);

		for (const auto& macro : macros)
		{
			stream << "|" << macro.name << "=" << macro.value;
		}

		return stream.str();
	}
}
