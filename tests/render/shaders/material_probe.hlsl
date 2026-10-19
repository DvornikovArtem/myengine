// A shader for the test of the material parameters (tests/render/MaterialRenderTests.cpp): two textures and two
// constants that a material sets by name. The colour of a pixel is  lerp(gTexture, gTexture2, gMix) * gColor + gBoost
// (the constants that the material leaves out are zero: no mix, no boost).

cbuffer DrawCB : register(b0)
{
    float4x4 gModel;
    float4x4 gViewProjection;
    float4 gColor;
    float4 gLightDirection;
    float gMix;
    float gBoost;
};

Texture2D gTexture : register(t0);
Texture2D gTexture2 : register(t1);
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
    output.Position = mul(mul(float4(input.Position, 1.0f), gModel), gViewProjection);
    output.UV = input.UV;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    const float4 first = gTexture.Sample(gSampler, input.UV);
    const float4 second = gTexture2.Sample(gSampler, input.UV);
    const float4 color = lerp(first, second, gMix) * gColor + gBoost;
    return float4(color.rgb, 1.0f);
}
