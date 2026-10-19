// A shader with every tag of the metadata comments (tests/render/ShaderMetadataTests.cpp).

cbuffer MaterialCB : register(b0)
{
    // @label "Roughness" @min 0 @max 1 @step 0.01 @default 0.5
    float gRoughness;
    float4 gTint;   // @color @label "Tint" @default 1 0.75 0.12 1
    // @label "Шероховатость, а не Roughness"
    // @min 0
    // An ordinary comment between the lines does not break them.
    // @max 10
    float gScale;
    float3 gGlow;   // @color @default 0, 0.5, 1
    float2 gUv;     // @default 2 3 @hidden
    int gMode;      // @min 0 @max 3 @default 1
    bool gFlag;     // @default true
    float gPlain;
    // @hidden
    float4x4 gModel;
#include "metadata_ok.hlsli"
};

// @label "Albedo map"
Texture2D gAlbedo : register(t0);
Texture2D gMask : register(t1);   // @hidden @label "Mask"
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position = mul(float4(input.Position, 1.0f), gModel);
    output.UV = input.UV;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 color = gAlbedo.Sample(gSampler, input.UV) * gMask.Sample(gSampler, input.UV) * gTint;
    color.rgb += gGlow * gRoughness * gScale * gShared + float3(gUv, gPlain) + gMode + gFlag;
    return color;
}
