// gColor with a @default (tests/render/ShaderMetadataTests.cpp): a material that says no tint gets it instead of white.

cbuffer DrawCB : register(b0)
{
    float4x4 gModel;
    float4x4 gViewProjection;
    // @color @label "Tint" @default 0.5 0.25 0.125 1
    float4 gColor;
};

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

float4 VSMain(float3 position : POSITION) : SV_POSITION
{
    return mul(mul(float4(position, 1.0f), gModel), gViewProjection);
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET
{
    return gTexture.Sample(gSampler, position.xy) * gColor;
}
