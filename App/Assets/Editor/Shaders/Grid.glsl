#shader vertex
#version 430 core

layout (location = 0) in vec3 geo_Pos;
layout (location = 1) in vec2 geo_TexCoords;
layout (location = 2) in vec3 geo_Normal;

layout (std140) uniform EngineUBO
{
    mat4    ubo_Model;
    mat4    ubo_View;
    mat4    ubo_Projection;
    vec3    ubo_ViewPos;
    float   ubo_Time;
};

out VS_OUT
{
    vec3 FragPos;
    vec2 TexCoords;
} vs_out;

void main()
{
    vs_out.FragPos      = vec3(ubo_Model * vec4(geo_Pos, 1.0));
    vs_out.TexCoords    = vs_out.FragPos.xz;

    gl_Position = ubo_Projection * ubo_View * vec4(vs_out.FragPos, 1.0);
}

#shader fragment
#version 430 core

out vec4 FRAGMENT_COLOR;

layout (std140) uniform EngineUBO
{
    mat4    ubo_Model;
    mat4    ubo_View;
    mat4    ubo_Projection;
    vec3    ubo_ViewPos;
    float   ubo_Time;
};

in VS_OUT
{
    vec3 FragPos;
    vec2 TexCoords;
} fs_in;

uniform vec3 u_Color;

float MAG(float p_lp)
{
  const float lineWidth = 1.0f;

  const vec2 coord       = fs_in.TexCoords / p_lp;
  const vec2 grid        = abs(fract(coord - 0.5) - 0.5) / fwidth(coord);
  const float line       = min(grid.x, grid.y);
  const float lineResult = lineWidth - min(line, lineWidth);

  return lineResult;
}

float GridLod(float height, float a, float b, float c)
{
  const float cl   = MAG(a);
  const float ml   = MAG(b);
  const float fl   = MAG(c);

  const float cmit =  16.0f;
  const float cmet =  52.0f;
  const float mfit =  96.0f;
  const float mfet =  196.0f;

  const float df   = clamp((height - cmit) / (cmet - cmit), 0.0f, 1.0f);
  const float dff  = clamp((height - mfit) / (mfet - mfit), 0.0f, 1.0f);

  const float inl  = mix(cl, ml, df);
  const float fnl  = mix(inl, fl, dff);

  return fnl;
}

float AxisMask(float spacing)
{
  const vec2 coord = fs_in.TexCoords / spacing;
  const vec2 axis = abs(coord) / max(fwidth(coord), vec2(1e-4));
  return 1.0f - min(min(axis.x, axis.y), 1.0f);
}

float SingleAxisMask(float coordinate, float spacing)
{
  const float coord = coordinate / spacing;
  const float axis = abs(coord) / max(fwidth(coord), 1e-4);
  return 1.0f - min(axis, 1.0f);
}

void main()
{
  const float height = distance(ubo_ViewPos.y, fs_in.FragPos.y);

  const float minorGrid = GridLod(height, 1.0f, 2.0f, 4.0f);
  const float midGrid   = GridLod(height, 5.0f, 10.0f, 20.0f);
  const float majorGrid = GridLod(height, 20.0f, 40.0f, 80.0f);

  const float minorHeightFade = 1.0f - smoothstep(14.0f, 56.0f, height);
  const float midHeightFade   = 1.0f - smoothstep(120.0f, 420.0f, height);
  const float majorHeightFade = 1.0f - smoothstep(700.0f, 1800.0f, height);

  const vec2  viewdirW    = ubo_ViewPos.xz - fs_in.FragPos.xz;
  const float viewdist    = length(viewdirW);
  const float minorDistanceFade = 1.0f - smoothstep(70.0f, 180.0f, viewdist);
  const float midDistanceFade   = 1.0f - smoothstep(180.0f, 560.0f, viewdist);
  const float majorDistanceFade = 1.0f - smoothstep(360.0f, 1400.0f, viewdist);
  const vec3  viewDir = normalize(ubo_ViewPos - fs_in.FragPos);
  const float angleFade = smoothstep(0.05f, 0.16f, abs(viewDir.y));

  const float minorContribution = minorGrid * 0.040f * minorHeightFade * minorDistanceFade;
  const float midContribution   = midGrid   * 0.145f * midHeightFade   * midDistanceFade;
  const float majorContribution = majorGrid * 0.54f  * majorHeightFade * majorDistanceFade;
  const float axisContribution  = AxisMask(20.0f) * 0.17f  * majorDistanceFade;
  const float xAxisContribution = SingleAxisMask(fs_in.TexCoords.x, 20.0f) * 0.030f * majorDistanceFade;
  const float zAxisContribution = SingleAxisMask(fs_in.TexCoords.y, 20.0f) * 0.030f * majorDistanceFade;

  const float minorAlpha = clamp(minorContribution, 0.0f, 1.0f);
  const float midAlpha   = clamp(midContribution,   0.0f, 1.0f);
  const float majorAlpha = clamp(majorContribution, 0.0f, 1.0f);
  const float axisAlpha  = clamp(axisContribution,  0.0f, 1.0f);
  const float xAxisAlpha = clamp(xAxisContribution, 0.0f, 1.0f);
  const float zAxisAlpha = clamp(zAxisContribution, 0.0f, 1.0f);

  const vec3 minorColor = u_Color * 0.74f;
  const vec3 midColor   = u_Color * 0.92f;
  const vec3 majorColor = u_Color * 1.18f;
  const vec3 axisColor  = u_Color * 1.16f;
  const vec3 xAxisColor = mix(axisColor, vec3(0.50f, 0.58f, 0.72f), 0.12f);
  const vec3 zAxisColor = mix(axisColor, vec3(0.60f, 0.67f, 0.52f), 0.10f);

  const vec3 layeredColor =
      minorColor * minorAlpha +
      midColor   * midAlpha +
      majorColor * majorAlpha +
      axisColor  * axisAlpha +
      xAxisColor * xAxisAlpha +
      zAxisColor * zAxisAlpha;

  const float totalAlpha = clamp(minorAlpha + midAlpha + majorAlpha + axisAlpha + xAxisAlpha + zAxisAlpha, 0.0f, 1.0f) * angleFade;

  FRAGMENT_COLOR = vec4(layeredColor, totalAlpha);
}
