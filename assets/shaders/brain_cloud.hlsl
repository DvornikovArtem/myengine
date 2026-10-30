// brain_cloud.hlsl: the cloud of glowing points (IRenderAdapter::DrawPointCloud, docs/flybrain/brain-view.md)
//
// One soft sprite per point, no vertex data but two streams of one record per point:
//   CLOUDPOS    xyz position, w colour index (0..15), static
//   CLOUDSPIKE  the time of the last flash of the point, rewritten every frame (a float of seconds), or with GPU_SPIKE the step of its last spike (a uint written by the
//               GPU brain, 0xFFFFFFFF: never) and the clock of the simulation in gGpu
// The flash is computed here from the age of the spike, exp(-age / decay), so the CPU only stores times, never fades anything.
// Drawn additively into an HDR target: where flashes pile up the value goes above 1 and the bloom picks it up.

cbuffer CloudCB : register(b0)
{
    float4x4 gViewProjection;
    float4 gViewport;    // xy: size of the target in pixels
    float4 gTiming;      // x: now, y: 1 / decay seconds, z: base brightness, w: flash brightness
    float4 gSizes;       // x: base size px, y: flash size px, z: reference depth, w: depth range
    float4 gPalette[16];
    uint4 gGpu;          // x: the step now, y: the part of a step (a float), z: the length of a step in seconds (a float)
};

struct VSInput
{
    float4 Point : CLOUDPOS;
#ifdef GPU_SPIKE
    uint Spike : CLOUDSPIKE;
#else
    float Spike : CLOUDSPIKE;
#endif
    uint VertexId : SV_VertexID;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
    float3 Color : TEXCOORD1;
};

static const float2 kCorners[6] = {float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(-1.0f, 1.0f), float2(-1.0f, 1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f)};

PSInput VSMain(VSInput input)
{
    PSInput output;
    const float2 corner = kCorners[input.VertexId % 6];
    float4 clip = mul(float4(input.Point.xyz, 1.0f), gViewProjection);

#ifdef GPU_SPIKE
    // the steps count in a uint that wraps (after 119 hours of model time); the difference of two of them is the age in steps
    float age = 1.0e6f;
    if (input.Spike != 0xFFFFFFFFu)
    {
        age = max((float) (int) (gGpu.x - input.Spike) + asfloat(gGpu.y), 0.0f) * asfloat(gGpu.z);
    }
#else
    const float age = max(gTiming.x - input.Spike, 0.0f);
#endif
    const float heat = exp(-age * gTiming.y);
    const float3 tint = gPalette[((uint) input.Point.w) & 15].rgb;

    // farther points are dimmer and smaller: the cloud gets depth without a single lit surface
    const float behind = saturate((clip.w - gSizes.z) / max(gSizes.w, 1e-3f) * 0.5f + 0.5f);
    const float depthFade = lerp(1.15f, 0.30f, behind);
    const float perspective = clamp(gSizes.z / max(clip.w, 1e-3f), 0.55f, 1.8f);

    float brightness = (gTiming.z + gTiming.w * heat) * depthFade;
    const float wantedSize = (gSizes.x + gSizes.y * heat) * perspective;
    // a sprite narrower than a pixel flickers: draw it at the minimum and give back the light it would have had
    const float drawSize = max(wantedSize, 1.8f);
    brightness *= (wantedSize * wantedSize) / (drawSize * drawSize);

    if (clip.w <= 1e-3f)
    {
        clip = float4(2.0f, 2.0f, 2.0f, 1.0f); // behind the camera: out of the picture
    }
    else
    {
        clip.xy += corner * drawSize * (2.0f / gViewport.xy) * clip.w;
    }
    output.Position = clip;
    output.UV = corner;
    output.Color = tint * min(brightness, 60.0f);
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    const float r2 = dot(input.UV, input.UV);
    float a = saturate(1.0f - r2);
    a *= a;
    return float4(input.Color * a, a);
}
