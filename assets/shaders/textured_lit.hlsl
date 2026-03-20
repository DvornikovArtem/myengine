cbuffer DrawCB : register(b0)
{
    float4x4 gModel;
    float4x4 gViewProjection;
    float4 gColor;
    float4 gLightDirection;
};

Texture2D gTexture : register(t0);
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
    float3 WorldNormal : TEXCOORD0;
    float2 UV : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 worldPosition = mul(float4(input.Position, 1.0f), gModel);
    output.Position = mul(worldPosition, gViewProjection);

    float3x3 normalMatrix = (float3x3) gModel;
    output.WorldNormal = mul(input.Normal, normalMatrix);
    output.UV = input.UV;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 normal = normalize(input.WorldNormal);
    float3 lightDir = normalize(-gLightDirection.xyz);
    float ndotl = saturate(dot(normal, lightDir));
    float lighting = gLightDirection.w + ndotl * (1.0f - gLightDirection.w);

    float4 texColor = gTexture.Sample(gSampler, input.UV);
    return texColor * gColor * lighting;
    //return texColor * gColor * 0.1f;
}
