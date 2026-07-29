// Basic color-only pipeline used for all debug/prototype geometry.

cbuffer PerDraw : register(b0)
{
    row_major float4x4 gMVP;
};

struct VSIn
{
    float3 pos   : POSITION;
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos   : SV_Position;
    float4 color : COLOR;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = mul(float4(v.pos, 1.0f), gMVP);
    o.color = v.color;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return i.color;
}
