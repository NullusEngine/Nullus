#pragma once

#include <string>
#include <vector>

#include "Rendering/Resources/ResourceBinding.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
	class Texture;

	class NLS_RENDER_API BindingSet
	{
	public:
		void Clear();
		void SetLayout(const ResourceBindingLayout& layout);
		void SetSampler(const std::string& name, const RHI::SamplerDesc& sampler);
		void SetTexture(const std::string& name, const Texture* texture);
		void SetBuffer(const std::string& name, const RHI::IRHIBuffer* buffer);
		void SetResource(const std::string& name, const RHI::IRHIResource* resource);
		const RHI::SamplerDesc* GetSampler(const std::string& name) const;
		const Texture* GetTexture(const std::string& name) const;
		const RHI::IRHIBuffer* GetBuffer(const std::string& name) const;
		const RHI::IRHIResource* GetResource(const std::string& name) const;
		const ResourceBindingEntry* Find(const std::string& name) const;
		const std::vector<ResourceBindingEntry>& Entries() const;

	private:
		std::vector<ResourceBindingEntry> m_entries;
	};
}
