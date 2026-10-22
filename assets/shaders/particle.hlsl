// particle.hlsl: billboards of the particle system (docs/render/particles.md)
//
// One quad (assets/models/fx/quad.obj) drawn once per particle: the instance stream (INSTANCE0..3, the same four float4 as the model
// matrix of the lit shader) carries the particle instead of a matrix:
//   INSTANCE0  xyz centre, w size in metres
//   INSTANCE1  xyz direction of the velocity, w stretch (0: faces the camera; > 0: a streak along the direction, this many times longer)
//   INSTANCE2  rgba colour (rgb may be above 1: it glows and blooms; alpha fades the particle)
//   INSTANCE3  x rotation in radians, y frame in the atlas (the atlas is gAtlasColumns frames side by side)
// The blend mode (additive or alpha) is the material's; the pass tests depth and does not write it.

cbuffer DrawCB : register(b0)
{
    // @hidden
    float4x4 gViewProjection;
    // @hidden
    float4 gCameraPosition;
    // @hidden
    float4 gCameraRight;
    // @hidden
    float4 gCameraUp;

    // @color @label "Tint" @default 1 1 1 1
    float4 gColor;
    // @label "Intensity" @min 0 @max 16 @step 0.05 @default 1
    float gIntensity;
    // @label "Atlas columns" @min 1 @max 16 @step 1 @default 6
    float gAtlasColumns;
};

// @label "Sprite atlas"
Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float4 Row0 : INSTANCE0;
    float4 Row1 : INSTANCE1;
    float4 Row2 : INSTANCE2;
    float4 Row3 : INSTANCE3;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
    float4 Color : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    const float3 center = input.Row0.xyz;
    const float size = input.Row0.w;
    const float stretch = input.Row1.w;
    const float2 corner = input.Position.xy; // -0.5 .. 0.5

    float3 world;
    if (stretch > 0.0f)
    {
        // a streak: the quad runs along the direction and turns its face to the camera
        const float3 axis = normalize(input.Row1.xyz + float3(0.0f, 1e-5f, 0.0f));
        const float3 toCamera = normalize(gCameraPosition.xyz - center);
        float3 side = cross(axis, toCamera);
        side = normalize(side + float3(1e-6f, 0.0f, 0.0f));
        world = center + axis * (corner.y * size * stretch) + side * (corner.x * size);
    }
    else
    {
        const float s = sin(input.Row3.x);
        const float c = cos(input.Row3.x);
        const float2 rotated = float2(c * corner.x - s * corner.y, s * corner.x + c * corner.y);
        world = center + gCameraRight.xyz * (rotated.x * size) + gCameraUp.xyz * (rotated.y * size);
    }

    output.Position = mul(float4(world, 1.0f), gViewProjection);
    output.UV = float2((input.Row3.y + input.UV.x) / max(gAtlasColumns, 1.0f), input.UV.y);
    output.Color = input.Row2;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    const float4 sprite = gTexture.Sample(gSampler, input.UV);
    const float4 tint = input.Color * gColor;
    // rgb above 1 stays above 1 (the scene target is HDR): the bloom picks it up
    return float4(sprite.rgb * tint.rgb * gIntensity, sprite.a * tint.a);
}
