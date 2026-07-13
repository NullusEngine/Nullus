#ifndef NLS_ENGINE_COMMON_TYPES_HLSLI
#define NLS_ENGINE_COMMON_TYPES_HLSLI

#if !defined(NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP)
struct VSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    float3 Normal : NORMAL;
    float3 Tangent : TEXCOORD1;
    float3 Bitangent : TEXCOORD2;
};

struct VSOutput
{
    float4 PositionCS : SV_Position;
    float3 PositionWS : TEXCOORD0;
    float3 NormalWS : TEXCOORD1;
    float3 TangentWS : TEXCOORD2;
    float3 BitangentWS : TEXCOORD3;
    float2 TexCoord : TEXCOORD4;
};
#endif

struct NLSTangentFrame
{
    float3 tangentWS;
    float3 bitangentWS;
    float3 normalWS;
};

#if !defined(NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP)
cbuffer ObjectIndexConstants : register(b1, space3)
{
    uint u_ObjectIndex;
    uint u_ObjectFlags;
    uint u_ObjectPadding0;
    uint u_ObjectPadding1;
};
static const uint NLS_OBJECT_FLAG_RECEIVE_SHADOWS = 1u;
static const uint NLS_OBJECT_FLAG_CAST_SHADOWS = 2u;
#endif

static const float NLS_SAFE_NORMAL_EPSILON = 1.0e-8f;
static const float NLS_SAFE_NORMAL_MAX_LENGTH_SQ = 1.0e+20f;
static const float NLS_SAFE_NORMAL_MAX_COMPONENT = 1.0e+30f;

#if !defined(NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP)
bool NLSIsFinite3(float3 value)
{
    return all(value == value) && all(abs(value) < NLS_SAFE_NORMAL_MAX_COMPONENT);
}
#endif

float3 NLSNormalizeFallback(float3 fallback)
{
    const float lengthSq = dot(fallback, fallback);
    if (NLSIsFinite3(fallback) && lengthSq > NLS_SAFE_NORMAL_EPSILON && lengthSq < NLS_SAFE_NORMAL_MAX_LENGTH_SQ)
        return fallback * rsqrt(lengthSq);

    return float3(0.0f, 0.0f, 1.0f);
}

#if !defined(NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP)
float3 NLSSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSq = dot(value, value);
    if (NLSIsFinite3(value) && lengthSq > NLS_SAFE_NORMAL_EPSILON && lengthSq < NLS_SAFE_NORMAL_MAX_LENGTH_SQ)
        return value * rsqrt(lengthSq);

    return NLSNormalizeFallback(fallback);
}
#endif

float3 NLSTransformNormalDirection(float3x3 model, float3 normal)
{
    const float3 fallback = mul(model, normal);
    const float3 row0 = float3(model._11, model._12, model._13);
    const float3 row1 = float3(model._21, model._22, model._23);
    const float3 row2 = float3(model._31, model._32, model._33);
    const float det = dot(row0, cross(row1, row2));
    const float3x3 cofactors = float3x3(
        cross(row1, row2),
        cross(row2, row0),
        cross(row0, row1));
    const float orientation = det < 0.0f ? -1.0f : 1.0f;
    const float3 transformed = abs(det) > NLS_SAFE_NORMAL_EPSILON
        ? mul(cofactors, normal) * orientation
        : fallback;
    return NLSSafeNormalize(transformed, fallback);
}

float3 NLSSafePerpendicular(float3 normalWS)
{
    const float3 safeNormalWS = NLSSafeNormalize(normalWS, float3(0.0f, 0.0f, 1.0f));
    const float3 reference = abs(safeNormalWS.z) < 0.999f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(0.0f, 1.0f, 0.0f);
    return NLSSafeNormalize(cross(reference, safeNormalWS), float3(1.0f, 0.0f, 0.0f));
}

NLSTangentFrame NLSBuildSafeTangentFrame(float3 normalWS, float3 tangentWS, float3 bitangentWS)
{
    NLSTangentFrame frame;
    frame.normalWS = NLSSafeNormalize(normalWS, float3(0.0f, 0.0f, 1.0f));

    const float3 tangentCandidate = tangentWS - frame.normalWS * dot(tangentWS, frame.normalWS);
    const float tangentLengthSq = dot(tangentCandidate, tangentCandidate);
    if (NLSIsFinite3(tangentCandidate) && tangentLengthSq > NLS_SAFE_NORMAL_EPSILON && tangentLengthSq < NLS_SAFE_NORMAL_MAX_LENGTH_SQ)
        frame.tangentWS = tangentCandidate * rsqrt(tangentLengthSq);
    else
        frame.tangentWS = NLSSafePerpendicular(frame.normalWS);

    const float3 bitangentCandidate =
        bitangentWS -
        frame.normalWS * dot(bitangentWS, frame.normalWS) -
        frame.tangentWS * dot(bitangentWS, frame.tangentWS);
    const float bitangentLengthSq = dot(bitangentCandidate, bitangentCandidate);
    if (NLSIsFinite3(bitangentCandidate) && bitangentLengthSq > NLS_SAFE_NORMAL_EPSILON && bitangentLengthSq < NLS_SAFE_NORMAL_MAX_LENGTH_SQ)
        frame.bitangentWS = bitangentCandidate * rsqrt(bitangentLengthSq);
    else
        frame.bitangentWS = NLSNormalizeFallback(cross(frame.normalWS, frame.tangentWS));

    return frame;
}

NLSTangentFrame NLSOrientTangentFrameForFace(NLSTangentFrame frame, bool isFrontFace)
{
    const float faceSign = isFrontFace ? 1.0f : -1.0f;
    frame.normalWS *= faceSign;
    frame.bitangentWS *= faceSign;
    return frame;
}

float3x3 NLSBuildTangentToWorldMatrix(NLSTangentFrame frame)
{
    return float3x3(frame.tangentWS, frame.bitangentWS, frame.normalWS);
}

float3 NLSApplyTangentNormal(float3 tangentNormal, NLSTangentFrame frame)
{
    return NLSSafeNormalize(mul(tangentNormal, NLSBuildTangentToWorldMatrix(frame)), frame.normalWS);
}

#endif
