cbuffer SpriteView : register(b0, space1) { row_major float4x4 SpriteVP; float4 SpriteMode; };
StructuredBuffer<float4> SpriteData : register(t0, space1);
Texture2D SpriteTexture : register(t1, space1);
SamplerState SpriteSampler : register(s0, space1);
cbuffer MaterialProperties : register(b0, space2) { float4 _BaseColor; };
struct Varyings { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
Varyings SpriteVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    Varyings output;
    float4 world;
    if (SpriteMode.x > 0.5)
    {
        // Draws start at zero on every backend. D3D SV_VertexID excludes the
        // API start vertex, so carry the shared-buffer range explicitly.
        uint offset = (vertex + (uint)SpriteMode.y * 6) * 3;
        world = SpriteData[offset]; output.uv = SpriteData[offset+1].xy; output.color = SpriteData[offset+2];
    }
    else
    {
        const uint corners[6] = {0,1,2,0,2,3}; uint corner = corners[vertex];
        uint offset = (instance + (uint)SpriteMode.y) * 6;
        world = SpriteData[offset+corner]; float4 uv = SpriteData[offset+4];
        output.uv = float2(corner < 2 ? uv.x : uv.z, corner == 0 || corner == 3 ? uv.w : uv.y);
        output.color = SpriteData[offset+5];
    }
    output.position = mul(SpriteVP,world);
    return output;
}
float4 SpritePS(Varyings input) : SV_Target0
{
    float4 color = SpriteTexture.Sample(SpriteSampler,input.uv) * input.color * _BaseColor;
    clip(color.a - 0.001);
    if (SpriteMode.z > 0.5) return float4(input.color.rgb,1);
    return color;
}
