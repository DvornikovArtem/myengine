Texture2D uiTexture : register(t0);
SamplerState uiSampler : register(s0);

cbuffer UiConstants : register(b0)
{
    float2 screenSize;
    float2 translation;
}

struct VSInput
{
    float2 position : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    const float2 pixel = input.position + translation;
    const float clipX = (pixel.x / max(screenSize.x, 1.0f)) * 2.0f - 1.0f;
    const float clipY = 1.0f - (pixel.y / max(screenSize.y, 1.0f)) * 2.0f;
    output.position = float4(clipX, clipY, 0.0f, 1.0f);
    output.color = input.color;
    output.uv = input.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    const float4 sampled = uiTexture.Sample(uiSampler, input.uv);
    return sampled * input.color;
}
