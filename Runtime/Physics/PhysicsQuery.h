#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

namespace NLS::Physics
{
struct QueryBufferResult
{
    size_t count = 0;
    size_t total = 0;
    bool truncated = false;
};

namespace Detail
{
// A caller-owned, bounded max heap retains the nearest hits without allocating.
// Empty output is also useful for counting required capacity.
template<class Hit>
class BoundedQueryResults
{
public:
    explicit BoundedQueryResults(std::span<Hit> output) : m_output(output) {}
    void Add(const Hit& hit)
    {
        ++m_result.total;
        if (m_output.empty()) return;
        if (m_result.count < m_output.size())
        {
            m_output[m_result.count++] = hit;
            std::push_heap(m_output.begin(), m_output.begin() + m_result.count, Less);
        }
        else if (Less(hit, m_output.front()))
        {
            std::pop_heap(m_output.begin(), m_output.end(), Less);
            m_output.back() = hit;
            std::push_heap(m_output.begin(), m_output.end(), Less);
        }
    }
    QueryBufferResult Finish()
    {
        if (m_result.count > 1) std::sort_heap(m_output.begin(), m_output.begin() + m_result.count, Less);
        m_result.truncated = m_result.count < m_result.total;
        return m_result;
    }
private:
    static bool Less(const Hit& a, const Hit& b)
    { return a.distance < b.distance || (a.distance == b.distance && a.shape < b.shape); }
    std::span<Hit> m_output;
    QueryBufferResult m_result;
};
}
}
