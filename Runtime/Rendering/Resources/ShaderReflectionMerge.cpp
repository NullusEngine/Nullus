#include "Rendering/Resources/ShaderReflectionMerge.h"

#include <algorithm>
#include <sstream>

namespace NLS::Render::Resources
{
    namespace
    {
        bool SamePropertyBinding(
            const ShaderPropertyDesc& lhs,
            const ShaderPropertyDesc& rhs)
        {
            return lhs.name == rhs.name &&
                lhs.type == rhs.type &&
                lhs.kind == rhs.kind &&
                lhs.bindingSpace == rhs.bindingSpace &&
                lhs.bindingIndex == rhs.bindingIndex &&
                lhs.location == rhs.location &&
                lhs.arraySize == rhs.arraySize &&
                lhs.byteOffset == rhs.byteOffset &&
                lhs.byteSize == rhs.byteSize &&
                lhs.parentConstantBuffer == rhs.parentConstantBuffer;
        }

        bool SameConstantBufferBinding(
            const ShaderConstantBufferDesc& lhs,
            const ShaderConstantBufferDesc& rhs)
        {
            return lhs.name == rhs.name &&
                lhs.bindingSpace == rhs.bindingSpace &&
                lhs.bindingIndex == rhs.bindingIndex;
        }

        bool SameCBufferMember(
            const ShaderCBufferMemberDesc& lhs,
            const ShaderCBufferMemberDesc& rhs)
        {
            return lhs.name == rhs.name &&
                lhs.type == rhs.type &&
                lhs.byteOffset == rhs.byteOffset &&
                lhs.byteSize == rhs.byteSize &&
                lhs.arraySize == rhs.arraySize;
        }

        std::string DescribeConstantBufferMember(
            const ShaderConstantBufferDesc& constantBuffer,
            const ShaderCBufferMemberDesc& member)
        {
            std::ostringstream stream;
            stream << constantBuffer.name << "." << member.name
                << " offset=" << member.byteOffset
                << " size=" << member.byteSize
                << " arraySize=" << member.arraySize
                << " cbufferSize=" << constantBuffer.byteSize;
            return stream.str();
        }

        bool ValidateConstantBufferMembers(
            const ShaderConstantBufferDesc& constantBuffer,
            std::string* diagnostic)
        {
            for (const auto& member : constantBuffer.members)
            {
                if (member.byteOffset > constantBuffer.byteSize ||
                    member.byteSize > constantBuffer.byteSize - member.byteOffset)
                {
                    if (diagnostic != nullptr)
                    {
                        *diagnostic = "Shader reflection member is outside its constant buffer: " +
                            DescribeConstantBufferMember(constantBuffer, member);
                    }
                    return false;
                }
            }
            return true;
        }

        bool HasAnyShaderReflectionData(const ShaderReflection& reflection)
        {
            return !reflection.properties.empty() ||
                !reflection.constantBuffers.empty();
        }

        bool MergeConstantBuffer(
            ShaderConstantBufferDesc& destination,
            const ShaderConstantBufferDesc& source,
            std::string* diagnostic)
        {
            if (!ValidateConstantBufferMembers(destination, diagnostic) ||
                !ValidateConstantBufferMembers(source, diagnostic))
            {
                return false;
            }

            destination.byteSize = std::max(destination.byteSize, source.byteSize);
            destination.stageMask = destination.stageMask | source.stageMask;
            for (const auto& member : source.members)
            {
                const auto sameName = std::find_if(
                    destination.members.begin(),
                    destination.members.end(),
                    [&member](const ShaderCBufferMemberDesc& existing)
                    {
                        return existing.name == member.name;
                    });
                if (sameName != destination.members.end())
                {
                    if (!SameCBufferMember(*sameName, member))
                    {
                        if (diagnostic != nullptr)
                        {
                            *diagnostic = "Shader reflection constant buffer member layout conflict: " +
                                DescribeConstantBufferMember(destination, *sameName) +
                                " vs " +
                                DescribeConstantBufferMember(source, member);
                        }
                        return false;
                    }
                    continue;
                }

                const auto sameLayout = std::find_if(
                    destination.members.begin(),
                    destination.members.end(),
                    [&member](const ShaderCBufferMemberDesc& existing)
                    {
                        return SameCBufferMember(existing, member);
                    });
                if (sameLayout == destination.members.end())
                    destination.members.push_back(member);
            }
            return true;
        }
    }

    bool TryMergeShaderReflection(
        ShaderReflection& destination,
        const ShaderReflection& source,
        std::string* diagnostic)
    {
        ShaderReflection merged = destination;
        for (const auto& property : source.properties)
        {
            const auto found = std::find_if(
                merged.properties.begin(),
                merged.properties.end(),
                [&property](const ShaderPropertyDesc& existing)
                {
                    return SamePropertyBinding(existing, property);
                });
            if (found == merged.properties.end())
            {
                merged.properties.push_back(property);
            }
            else
            {
                found->stageMask = found->stageMask | property.stageMask;
            }
        }

        for (const auto& constantBuffer : source.constantBuffers)
        {
            if (!ValidateConstantBufferMembers(constantBuffer, diagnostic))
                return false;

            const auto found = std::find_if(
                merged.constantBuffers.begin(),
                merged.constantBuffers.end(),
                [&constantBuffer](const ShaderConstantBufferDesc& existing)
                {
                    return SameConstantBufferBinding(existing, constantBuffer);
                });
            if (found == merged.constantBuffers.end())
            {
                merged.constantBuffers.push_back(constantBuffer);
                continue;
            }

            if (!MergeConstantBuffer(*found, constantBuffer, diagnostic))
                return false;
        }

        destination = std::move(merged);
        return true;
    }

    bool TryMergePreferredShaderReflectionOrFallback(
        std::span<const ShaderReflection> preferredStages,
        std::span<const ShaderReflection> fallbackStages,
        ShaderReflection& destination,
        std::string* diagnostic)
    {
        auto mergeStages = [diagnostic](std::span<const ShaderReflection> stages, ShaderReflection& merged)
        {
            for (const auto& stage : stages)
            {
                if (!TryMergeShaderReflection(merged, stage, diagnostic))
                    return false;
            }
            return true;
        };

        ShaderReflection preferred;
        if (!mergeStages(preferredStages, preferred))
            return false;

        if (HasAnyShaderReflectionData(preferred) || fallbackStages.empty())
        {
            destination = std::move(preferred);
            return true;
        }

        ShaderReflection fallback;
        if (!mergeStages(fallbackStages, fallback))
            return false;

        destination = std::move(fallback);
        return true;
    }

    void MergeShaderReflection(
        ShaderReflection& destination,
        const ShaderReflection& source)
    {
        std::string diagnostic;
        (void)TryMergeShaderReflection(destination, source, &diagnostic);
    }
}
