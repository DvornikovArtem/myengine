// Every mistake of the metadata comments, once (tests/render/ShaderMetadataTests.cpp finds the lines by the names).

cbuffer ErrorCB : register(b0)
{
    float gBadTag;      // @frobnicate 1
    float gBadDefault;  // @default abc
    float3 gShort;      // @default 1 2
    float gRange;       // @min 5 @max 1
    float gOutside;     // @min 0 @max 1 @default 2
    float gNotColor;    // @color
    float gNoQuotes;    // @label Roughness
    // @label "separated by a blank line"

    float gSeparated;
    float gFine;        // @label "fine" @min 0 @max 1 @default 0.5
#include "metadata_bad.hlsli"
};

// @label "this texture is not bound"
Texture2D gUnused : register(t5);
Texture2D gUsed : register(t0);   // @min 0
SamplerState gSampler : register(s0);

float4 VSMain(float3 position : POSITION) : SV_POSITION
{
    return float4(position, 1.0f) * (gBadTag + gBadDefault + gShort.x + gRange + gOutside + gNotColor + gNoQuotes + gSeparated + gFine + gIncludeBad);
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET
{
    return gUsed.Sample(gSampler, position.xy);
}

// @hidden

// @hidden
float4 Unused(float4 value) { return value; }
