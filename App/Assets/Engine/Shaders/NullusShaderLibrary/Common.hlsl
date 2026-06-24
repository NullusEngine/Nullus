#ifndef NULLUS_SHADER_LIBRARY_COMMON_INCLUDED
#define NULLUS_SHADER_LIBRARY_COMMON_INCLUDED

static const float NLS_SAFE_EPSILON = 1.0e-8f;
static const float NLS_SAFE_MAX_LENGTH_SQ = 1.0e+20f;
static const float NLS_SAFE_MAX_COMPONENT = 1.0e+30f;

bool NLSIsFinite3(float3 value)
{
    return all(value == value) && all(abs(value) < NLS_SAFE_MAX_COMPONENT);
}

float3 NLSSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSq = dot(value, value);
    if (NLSIsFinite3(value) && lengthSq > NLS_SAFE_EPSILON && lengthSq < NLS_SAFE_MAX_LENGTH_SQ)
        return value * rsqrt(lengthSq);
    return fallback;
}

#endif
