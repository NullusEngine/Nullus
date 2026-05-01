#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Rendering/Resources/ResourceBinding.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
	class NLS_RENDER_API BindingSet
	{
	public:
		void Clear();
		void SetLayout(const ResourceBindingLayout& layout);
		void SetSampler(const std::string& name, const RHI::SamplerDesc& sampler);
		void SetTexture(const std::string& name, const std::shared_ptr<RHI::RHITexture>& texture);
		void SetBuffer(const std::string& name, const std::shared_ptr<RHI::RHIBuffer>& buffer);
		const RHI::SamplerDesc* GetSampler(const std::string& name) const;

	private:
		std::vector<ResourceBindingEntry> m_entries;
	};
}
