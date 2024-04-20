#include "Debug.h"
#include "Matrix4.h"
using namespace NLS;

OGLRenderer* RendererDebug::renderer = nullptr;

std::vector<RendererDebug::DebugStringEntry> RendererDebug::stringEntries;
std::vector<RendererDebug::DebugLineEntry> RendererDebug::lineEntries;

const Vector4 RendererDebug::RED = Vector4(1, 0, 0, 1);
const Vector4 RendererDebug::GREEN = Vector4(0, 1, 0, 1);
const Vector4 RendererDebug::BLUE = Vector4(0, 0, 1, 1);

const Vector4 RendererDebug::BLACK = Vector4(0, 0, 0, 1);
const Vector4 RendererDebug::WHITE = Vector4(1, 1, 1, 1);

const Vector4 RendererDebug::YELLOW = Vector4(1, 1, 0, 1);
const Vector4 RendererDebug::MAGENTA = Vector4(1, 0, 1, 1);
const Vector4 RendererDebug::CYAN = Vector4(0, 1, 1, 1);


void RendererDebug::Print(const std::string& text, const Vector2& pos, const Vector4& colour)
{
    DebugStringEntry newEntry;

    newEntry.data = text;
    newEntry.position = pos;
    newEntry.colour = colour;

    stringEntries.emplace_back(newEntry);
}

void RendererDebug::DrawLine(const Vector3& startpoint, const Vector3& endpoint, const Vector4& colour, float time)
{
    DebugLineEntry newEntry;

    newEntry.start = startpoint;
    newEntry.end = endpoint;
    newEntry.colour = colour;
    newEntry.time = time;

    lineEntries.emplace_back(newEntry);
}

void RendererDebug::DrawAxisLines(const Matrix4& modelMatrix, float scaleBoost, float time)
{
    Matrix4 local = modelMatrix;
    local.SetPositionVector({0, 0, 0});

    Vector3 fwd = local * Vector4(0, 0, -1, 1.0f);
    Vector3 up = local * Vector4(0, 1, 0, 1.0f);
    Vector3 right = local * Vector4(1, 0, 0, 1.0f);

    Vector3 worldPos = modelMatrix.GetPositionVector();

    DrawLine(worldPos, worldPos + (right * scaleBoost), RendererDebug::RED, time);
    DrawLine(worldPos, worldPos + (up * scaleBoost), RendererDebug::GREEN, time);
    DrawLine(worldPos, worldPos + (fwd * scaleBoost), RendererDebug::BLUE, time);
}


void RendererDebug::FlushRenderables(float dt)
{
    if (!renderer)
    {
        return;
    }
    for (const auto& i : stringEntries)
    {
        renderer->DrawString(i.data, i.position);
    }
    int trim = 0;
    for (int i = 0; i < lineEntries.size();)
    {
        DebugLineEntry* e = &lineEntries[i];
        renderer->DrawLine(e->start, e->end, e->colour);
        e->time -= dt;
        if (e->time < 0)
        {
            trim++;
            lineEntries[i] = lineEntries[lineEntries.size() - trim];
        }
        else
        {
            ++i;
        }
        if (i + trim >= lineEntries.size())
        {
            break;
        }
    }
    lineEntries.resize(lineEntries.size() - trim);

    stringEntries.clear();
}