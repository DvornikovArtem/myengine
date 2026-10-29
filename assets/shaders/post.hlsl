// post.hlsl: the fullscreen passes of the scene (docs/render/scene-passes.md): the bloom and the composite into the back buffer.
// One triangle, no vertex buffer. gPostParams.x picks the pass:
//   0  bright pass: the part of the scene above the threshold, at half size
//   1  downsample (4 bilinear taps = 4x4 texels)
//   2  upsample (a tent of 4 taps), added into the larger level by the blend state
//   3  composite: scene + bloom * intensity
//   4  composite without bloom (the resolved scene)
//   5  composite with a tone map: 1 - exp(-x * gPostParams.w), the scene + bloom (point clouds, docs/flybrain/brain-view.md)
//   6  the same without bloom

cbuffer PostCB : register(b0)
{
    // x: pass, y: bloom threshold, z: bloom intensity, w: exposure (passes 5 and 6)
    float4 gPostParams;
    // xy: the texel of the source texture
    float4 gTexelSize;
};

Texture2D gSource : register(t0);
Texture2D gBloom : register(t1);
SamplerState gLinearSampler : register(s0);

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.UV = uv;
    output.Position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float3 Tap(Texture2D source, float2 uv)
{
    return source.SampleLevel(gLinearSampler, uv, 0).rgb;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const float2 texel = gTexelSize.xy;
    const int mode = (int) (gPostParams.x + 0.5f);

    if (mode == 0)
    {
        // soft threshold on each tap: a single hot pixel does not flicker the whole bloom
        const float threshold = gPostParams.y;
        const float2 o = texel;
        float3 sum = 0.0f;
        const float2 offsets[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const float3 c = min(Tap(gSource, input.UV + offsets[i] * o), 16.0f);
            sum += max(c - threshold, 0.0f);
        }
        return float4(sum * 0.25f, 1.0f);
    }
    if (mode == 1)
    {
        float3 sum = 0.0f;
        const float2 offsets[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            sum += Tap(gSource, input.UV + offsets[i] * texel);
        }
        return float4(sum * 0.25f, 1.0f);
    }
    if (mode == 2)
    {
        float3 sum = 0.0f;
        const float2 offsets[4] = {float2(-0.5f, -0.5f), float2(0.5f, -0.5f), float2(-0.5f, 0.5f), float2(0.5f, 0.5f)};
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            sum += Tap(gSource, input.UV + offsets[i] * texel);
        }
        return float4(sum * 0.25f, 1.0f);
    }

    // The composite writes the pixels of the region the scene was drawn into, so the source is read at the same pixel of the
    // surface (not over the whole triangle): the texel size here is that of the surface
    const float2 pixelUV = input.Position.xy * texel;
    const float3 scene = Tap(gSource, pixelUV);
    if (mode == 3)
    {
        const float3 bloom = Tap(gBloom, pixelUV);
        return float4(scene + bloom * gPostParams.z, 1.0f);
    }
    if (mode == 5 || mode == 6)
    {
        const float3 hdr = scene + (mode == 5 ? Tap(gBloom, pixelUV) * gPostParams.z : 0.0f);
        return float4(1.0f - exp(-max(hdr, 0.0f) * gPostParams.w), 1.0f);
    }
    return float4(scene, 1.0f);
}
