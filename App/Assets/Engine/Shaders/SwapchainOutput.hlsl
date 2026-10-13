struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

Texture2D SceneColorTexture : register(t0, space0);
SamplerState SceneColorSampler : register(s1, space0);

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.UV = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return SceneColorTexture.Sample(SceneColorSampler, input.UV);
}
