#shader vertex
#version 430 core

layout (location = 0) in vec3 geo_Pos;
layout (location = 1) in vec2 geo_TexCoords;
layout (location = 2) in vec3 geo_Normal;
layout (location = 3) in vec3 geo_Tangent;
layout (location = 4) in vec3 geo_Bitangent;

layout (std140, binding = 0) uniform EngineUBO
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
    vec3 Normal;
    vec2 TexCoords;
    mat3 TBN;
} vs_out;

void main()
{
    vec3 tangent = normalize(vec3(ubo_Model * vec4(geo_Tangent, 0.0)));
    vec3 bitangent = normalize(vec3(ubo_Model * vec4(geo_Bitangent, 0.0)));
    vec3 normal = normalize(vec3(ubo_Model * vec4(geo_Normal, 0.0)));

    vs_out.TBN = mat3(tangent, bitangent, normal);
    vs_out.FragPos = vec3(ubo_Model * vec4(geo_Pos, 1.0));
    vs_out.Normal = normalize(mat3(transpose(inverse(ubo_Model))) * geo_Normal);
    vs_out.TexCoords = geo_TexCoords;

    gl_Position = ubo_Projection * ubo_View * vec4(vs_out.FragPos, 1.0);
}

#shader fragment
#version 430 core

layout (location = 0) out vec4 g_Albedo;
layout (location = 1) out vec4 g_Position;
layout (location = 2) out vec4 g_Normal;
layout (location = 3) out vec4 g_Material;

in VS_OUT
{
    vec3 FragPos;
    vec3 Normal;
    vec2 TexCoords;
    mat3 TBN;
} fs_in;

uniform vec2        u_TextureTiling           = vec2(1.0, 1.0);
uniform vec2        u_TextureOffset           = vec2(0.0, 0.0);
uniform vec4        u_Diffuse                 = vec4(1.0, 1.0, 1.0, 1.0);
uniform vec3        u_Specular                = vec3(1.0, 1.0, 1.0);
uniform float       u_Shininess               = 100.0;
uniform bool        u_EnableNormalMapping     = false;
uniform sampler2D   u_DiffuseMap;
uniform sampler2D   u_SpecularMap;
uniform sampler2D   u_NormalMap;
uniform sampler2D   u_MaskMap;

void main()
{
    vec2 texCoords = u_TextureOffset + vec2(
        mod(fs_in.TexCoords.x * u_TextureTiling.x, 1.0),
        mod(fs_in.TexCoords.y * u_TextureTiling.y, 1.0)
    );

    if (texture(u_MaskMap, texCoords).r == 0.0)
    {
        discard;
    }

    vec4 diffuseTexel = texture(u_DiffuseMap, texCoords) * u_Diffuse;
    vec3 normal = normalize(fs_in.Normal);
    if (u_EnableNormalMapping)
    {
        vec3 mappedNormal = texture(u_NormalMap, texCoords).rgb;
        mappedNormal = normalize(mappedNormal * 2.0 - 1.0);
        normal = normalize(fs_in.TBN * mappedNormal);
    }

    vec3 specularTexel = texture(u_SpecularMap, texCoords).rgb * u_Specular;

    g_Albedo = diffuseTexel;
    g_Position = vec4(fs_in.FragPos, 1.0);
    g_Normal = vec4(normalize(normal) * 0.5 + 0.5, 1.0);
    g_Material = vec4(specularTexel, clamp(u_Shininess / 256.0, 0.0, 1.0));
}
