#ifndef NLS_SELECTION_OUTLINE_COMPOSITE_CORE_HLSLI
#define NLS_SELECTION_OUTLINE_COMPOSITE_CORE_HLSLI

struct SelectionOutlineEdgeSample
{
    float edge;
    float occlusionFade;
    float classification;
};

struct SelectionOutlineSoftOutline
{
    float outline;
    float occlusionFade;
    float classification;
};

struct SelectionOutlineMaskNeighborhood
{
    float4 center;
    float4 left;
    float4 right;
    float4 up;
    float4 down;
    float4 left2;
    float4 right2;
    float4 up2;
    float4 down2;
    float4 leftUp;
    float4 leftDown;
    float4 rightUp;
    float4 rightDown;
};

SelectionOutlineMaskNeighborhood BuildSelectionOutlineMaskNeighborhood(float2 uv)
{
    const float2 texel = float2(u_TexelSize.x, u_TexelSize.y);
    SelectionOutlineMaskNeighborhood neighborhood;
    neighborhood.center = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv);
    neighborhood.left = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(-texel.x, 0.0f));
    neighborhood.right = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(texel.x, 0.0f));
    neighborhood.up = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(0.0f, -texel.y));
    neighborhood.down = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(0.0f, texel.y));
    neighborhood.left2 = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(-2.0f * texel.x, 0.0f));
    neighborhood.right2 = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(2.0f * texel.x, 0.0f));
    neighborhood.up2 = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(0.0f, -2.0f * texel.y));
    neighborhood.down2 = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(0.0f, 2.0f * texel.y));
    neighborhood.leftUp = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(-texel.x, -texel.y));
    neighborhood.leftDown = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(-texel.x, texel.y));
    neighborhood.rightUp = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(texel.x, -texel.y));
    neighborhood.rightDown = u_SelectionOutlineMask.Sample(u_LinearClampSampler, uv + float2(texel.x, texel.y));
    return neighborhood;
}

float4 SelectionOutlineMaskChooseEdgeSource(float4 centerMask, float4 neighborMask)
{
    return SelectionOutlineMaskGetSelected(neighborMask) > SelectionOutlineMaskGetSelected(centerMask)
        ? neighborMask
        : centerMask;
}

float SelectionOutlineMaskSourceOcclusion(float4 sourceMask)
{
    return SelectionOutlineMaskGetSelected(sourceMask) > 0.5f
        ? saturate(SelectionOutlineMaskGetSelected(sourceMask) - SelectionOutlineMaskGetVisible(sourceMask))
        : 0.0f;
}

void AccumulateSelectionOutlineEdge(
    inout float edgeSum,
    inout float occlusionSum,
    inout float classificationSum,
    float4 centerMask,
    float4 neighborMask)
{
    const float groupEdge = abs(
        SelectionOutlineMaskGetGroupId(centerMask) -
        SelectionOutlineMaskGetGroupId(neighborMask));
    const float classificationEdge = abs(
        SelectionOutlineMaskGetClassification(centerMask) -
        SelectionOutlineMaskGetClassification(neighborMask));
    const float edge = saturate(max(groupEdge, classificationEdge));
    const float4 sourceMask = SelectionOutlineMaskChooseEdgeSource(centerMask, neighborMask);
    edgeSum += edge;
    occlusionSum += edge * SelectionOutlineMaskSourceOcclusion(sourceMask);
    classificationSum += edge * SelectionOutlineMaskGetClassification(sourceMask);
}

SelectionOutlineEdgeSample ComputeIdEdgeFromMasks(
    float4 centerMask,
    float4 leftMask,
    float4 rightMask,
    float4 upMask,
    float4 downMask)
{
    float edgeSum = 0.0f;
    float occlusionSum = 0.0f;
    float classificationSum = 0.0f;
    AccumulateSelectionOutlineEdge(edgeSum, occlusionSum, classificationSum, centerMask, leftMask);
    AccumulateSelectionOutlineEdge(edgeSum, occlusionSum, classificationSum, centerMask, rightMask);
    AccumulateSelectionOutlineEdge(edgeSum, occlusionSum, classificationSum, centerMask, upMask);
    AccumulateSelectionOutlineEdge(edgeSum, occlusionSum, classificationSum, centerMask, downMask);

    SelectionOutlineEdgeSample result;
    result.edge = saturate(edgeSum);
    result.occlusionFade = edgeSum > 0.0001f ? saturate(occlusionSum / edgeSum) : 0.0f;
    result.classification = edgeSum > 0.0001f ? saturate(classificationSum / edgeSum) : 0.0f;
    return result;
}

void AccumulateSelectionOutlineSoftEdgeSample(
    inout float outline,
    inout float occlusionSum,
    inout float classificationSum,
    SelectionOutlineEdgeSample edgeSample,
    float weight)
{
    const float weightedEdge = edgeSample.edge * weight;
    outline += weightedEdge;
    occlusionSum += weightedEdge * edgeSample.occlusionFade;
    classificationSum += weightedEdge * edgeSample.classification;
}

SelectionOutlineSoftOutline ComputeSoftOutline(float2 uv)
{
    float outline = 0.0f;
    float occlusionSum = 0.0f;
    float classificationSum = 0.0f;
    const SelectionOutlineMaskNeighborhood neighborhood = BuildSelectionOutlineMaskNeighborhood(uv);
    AccumulateSelectionOutlineSoftEdgeSample(
        outline,
        occlusionSum,
        classificationSum,
        ComputeIdEdgeFromMasks(
            neighborhood.center,
            neighborhood.left,
            neighborhood.right,
            neighborhood.up,
            neighborhood.down),
        0.36f);
    AccumulateSelectionOutlineSoftEdgeSample(
        outline,
        occlusionSum,
        classificationSum,
        ComputeIdEdgeFromMasks(
            neighborhood.right,
            neighborhood.center,
            neighborhood.right2,
            neighborhood.rightUp,
            neighborhood.rightDown),
        0.16f);
    AccumulateSelectionOutlineSoftEdgeSample(
        outline,
        occlusionSum,
        classificationSum,
        ComputeIdEdgeFromMasks(
            neighborhood.left,
            neighborhood.left2,
            neighborhood.center,
            neighborhood.leftUp,
            neighborhood.leftDown),
        0.16f);
    AccumulateSelectionOutlineSoftEdgeSample(
        outline,
        occlusionSum,
        classificationSum,
        ComputeIdEdgeFromMasks(
            neighborhood.down,
            neighborhood.leftDown,
            neighborhood.rightDown,
            neighborhood.center,
            neighborhood.down2),
        0.16f);
    AccumulateSelectionOutlineSoftEdgeSample(
        outline,
        occlusionSum,
        classificationSum,
        ComputeIdEdgeFromMasks(
            neighborhood.up,
            neighborhood.leftUp,
            neighborhood.rightUp,
            neighborhood.up2,
            neighborhood.center),
        0.16f);

    SelectionOutlineSoftOutline result;
    result.outline = saturate(outline);
    result.occlusionFade = outline > 0.0001f ? saturate(occlusionSum / outline) : 0.0f;
    result.classification = outline > 0.0001f ? saturate(classificationSum / outline) : 0.0f;
    return result;
}

float4 Composite(VSOutput input) : SV_Target0
{
    const float2 uv = input.TexCoord;
    const SelectionOutlineSoftOutline softOutline = ComputeSoftOutline(uv);
    const float child = step(0.5f, softOutline.classification);
    const float4 color = lerp(u_OutlineColor, u_ChildOutlineColor, child);
    const float outlineAlpha = saturate(softOutline.outline) * lerp(1.0f, 0.45f, softOutline.occlusionFade);
    clip(outlineAlpha - 0.001f);
    return float4(color.rgb, outlineAlpha);
}

#endif
