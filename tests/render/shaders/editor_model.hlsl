// A shader with one of every kind of row of the Material Editor (tests/render/MaterialEditorModelTests.cpp).

cbuffer EditorCB : register(b0)
{
    float4x4 gModel;
    // @label "Roughness" @min 0 @max 1 @step 0.01 @default 0.5
    float gRoughness;
    // @color @label "Tint" @default 1 0.75 0.12 1
    float4 gColor;
    // @color @default 0.2 0.4 0.6
    float3 gGlow;
    float2 gOffset;   // @min -1 @max 1 @step 0.25
    // @min 0 @max 10
    int gLevels;
    uint gCount;      // @max 100
    bool gEnabled;
    float gGain;      // @min 0
    float gLoose;
    // @hidden
    float gSecret;
    float4x4 gExtra;
    int3 gCells;
    float gWeights[2];
};

Texture2D gAlbedo : register(t1);
// @label "Detail"
Texture2D gDetail : register(t2);
Texture2D gBase : register(t0);
// @hidden
Texture2D gMask : register(t3);
SamplerState gSampler : register(s0);

float4 VSMain(float3 position : POSITION) : SV_POSITION
{
    return mul(float4(position, 1.0f), gModel) * (gRoughness + gGlow.x + gOffset.x + gLevels + gCount + gEnabled + gGain + gLoose + gSecret + gCells.x + gWeights[1]) +
        gExtra[0];
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET
{
    return gBase.Sample(gSampler, position.xy) * gAlbedo.Sample(gSampler, position.xy) * gDetail.Sample(gSampler, position.xy) * gMask.Sample(gSampler, position.xy) * gColor;
}
