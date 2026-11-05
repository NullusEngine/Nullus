#include "ComponentCodec.h"
#include <algorithm>
#include <cmath>

namespace NLS::ECS
{
void ValidateComponentJson(const ComponentJson& value, size_t maximumDepth)
{
    std::vector<std::pair<const ComponentJson*, size_t>> pending{{&value, 0}};
    while (!pending.empty())
    {
        const auto [item, depth] = pending.back(); pending.pop_back();
        if (depth > maximumDepth) throw std::length_error("ECS JSON nesting limit exceeded");
        if (item->is_discarded() || item->is_binary() || (item->is_number_float() && !std::isfinite(item->get<double>())))
            throw std::invalid_argument("ECS archive contains a nonportable JSON value");
        if (item->is_structured()) for (const auto& child : *item) pending.emplace_back(&child, depth + 1);
    }
}
std::string EntityWriteContext::EntityId(Entity entity) const
{
    if (entity == Entity{}) return {};
    const auto found = m_entities.find(entity);
    if (found == m_entities.end()) throw std::invalid_argument("ECS reference points outside the saved document");
    return found->second;
}
std::string EntityWriteContext::Resource(std::string guid, std::string hashOrVersion) const
{
    if (guid.empty()) return {};
    for (const auto& dependency : m_dependencies)
        if (dependency.guid == guid)
        {
            if (dependency.hashOrVersion != hashOrVersion) throw std::invalid_argument("Conflicting ECS resource dependency: " + guid);
            return guid;
        }
    m_dependencies.push_back({guid, std::move(hashOrVersion)}); return guid;
}
Entity EntityReadContext::Resolve(std::string_view localId) const
{
    if (localId.empty()) return {};
    const auto found = m_entities.find(std::string(localId));
    if (found == m_entities.end()) throw std::invalid_argument("Unresolved ECS document entity reference: " + std::string(localId));
    return found->second;
}
std::string EntityReadContext::Resource(std::string_view guid) const
{
    if (guid.empty()) return {};
    if (std::none_of(m_dependencies.begin(), m_dependencies.end(), [&](const auto& item) { return item.guid == guid; }))
        throw std::invalid_argument("Untracked ECS resource GUID: " + std::string(guid));
    return std::string(guid);
}
}
