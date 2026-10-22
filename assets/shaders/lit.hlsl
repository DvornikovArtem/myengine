// lit.hlsl: the lit program of the scene (docs/render/scene-passes.md)
//
//   - the model matrix of each instance comes from the second vertex stream (INSTANCE0..3), so draws of one mesh and material are one draw
//   - the sun with a shadow map (PCF 3x3), a hemisphere ambient (sky above, ground below), a Blinn-Phong highlight, a fresnel rim
//   - emissive (the colour of the texture times gEmissiveStrength, plus gEmissiveColor): above 1 it blooms; gUnlit skips the lighting
//   - gAlphaCutoff > 0: alpha test (nets, fences); the blend mode (alpha, additive) is the material's "blend"
//
// The values marked @hidden are written by the renderer by name, the others come from the material.

cbuffer DrawCB : register(b0)
{
    // @hidden
    float4x4 gViewProjection;
    // @hidden
    float4x4 gLightViewProjection;
    // @hidden
    float4 gCameraPosition;
    // @hidden
    float4 gLightDirection;
    // @hidden
    float4 gLightColor;
    // @hidden
    float4 gAmbientSky;
    // @hidden
    float4 gAmbientGround;
    // @hidden
    float4 gShadowParams;

    // @color @label "Tint" @default 1 1 1 1
    float4 gColor;
    // @color @label "Emissive colour" @default 0 0 0 0
    float4 gEmissiveColor;
    // @label "Specular" @min 0 @max 4 @step 0.01 @default 0.25
    float gSpecular;
    // @label "Gloss" @min 1 @max 256 @step 1 @default 32
    float gGloss;
    // @label "Fresnel rim" @min 0 @max 2 @step 0.01 @default 0.25
    float gFresnel;
    // @label "Emissive strength" @min 0 @max 16 @step 0.05 @default 0
    float gEmissiveStrength;
    // @label "Alpha cutoff" @min 0 @max 1 @step 0.01 @default 0
    float gAlphaCutoff;
    // @label "Unlit" @min 0 @max 1 @step 1 @default 0
    float gUnlit;
};

// @label "Albedo"
Texture2D gTexture : register(t0);
// @hidden
Texture2D<float> gShadowMap : register(t1);
SamplerState gSampler : register(s0);
SamplerComparisonState gShadowSampler : register(s1);

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
    float3 WorldPosition : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float4 LightClip : TEXCOORD3;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    const float4x4 model = float4x4(input.Row0, input.Row1, input.Row2, input.Row3);
    const float4 worldPosition = mul(float4(input.Position, 1.0f), model);
    output.Position = mul(worldPosition, gViewProjection);
    output.WorldPosition = worldPosition.xyz;

    const float3 worldNormal = normalize(mul(input.Normal, (float3x3) model));
    output.WorldNormal = worldNormal;
    output.UV = input.UV;

    // The position in the light's clip space, moved along the normal a little: the acne of a surface at a slant is gone
    output.LightClip = mul(float4(worldPosition.xyz + worldNormal * gShadowParams.z, 1.0f), gLightViewProjection);
    return output;
}

// The share of the light that reaches the point: 1 lit, 0 in shadow. 3x3 taps of the comparison sampler.
float ShadowFactor(float4 lightClip)
{
    if (gShadowParams.w < 0.5f)
    {
        return 1.0f;
    }
    const float3 ndc = lightClip.xyz / lightClip.w;
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || ndc.z > 1.0f)
    {
        return 1.0f;
    }

    const float depth = ndc.z - gShadowParams.y;
    const float texel = gShadowParams.x;
    float sum = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            sum += gShadowMap.SampleCmpLevelZero(gShadowSampler, uv + float2(x, y) * texel, depth);
        }
    }
    return sum / 9.0f;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    const float4 albedo = gTexture.Sample(gSampler, input.UV) * gColor;
    if (gAlphaCutoff > 0.0f)
    {
        clip(albedo.a - gAlphaCutoff);
    }

    const float3 normal = normalize(input.WorldNormal);
    const float3 toLight = normalize(-gLightDirection.xyz);
    const float3 toEye = normalize(gCameraPosition.xyz - input.WorldPosition);
    const float3 halfVector = normalize(toLight + toEye);

    const float nDotL = saturate(dot(normal, toLight));
    const float shadow = ShadowFactor(input.LightClip);
    const float3 hemisphere = lerp(gAmbientGround.rgb, gAmbientSky.rgb, normal.y * 0.5f + 0.5f);

    // Blinn-Phong: the highlight of the sun, only where it shines
    const float highlight = pow(saturate(dot(normal, halfVector)), gGloss) * gSpecular * nDotL * shadow;
    // Fresnel: the edges that look away from the eye take the colour of the sky
    const float rim = pow(1.0f - saturate(dot(normal, toEye)), 4.0f) * gFresnel;

    const float3 diffuse = albedo.rgb * (hemisphere + gLightColor.rgb * nDotL * shadow);
    const float3 lit = diffuse + gLightColor.rgb * highlight + gAmbientSky.rgb * rim * 2.0f;
    const float3 emissive = albedo.rgb * gEmissiveStrength + gEmissiveColor.rgb;

    return float4(lerp(lit, albedo.rgb, gUnlit) + emissive, albedo.a);
}
