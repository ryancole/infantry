// The sun's depth pass. Position and nothing else: this pipeline has no pixel
// shader and no render target, so the only thing it produces is how far the
// nearest surface is from the sun along each of the shadow map's texels.
//
// Declaring only POSITION is what lets one pipeline swallow every caster in the
// arena. The soldiers and props arrive as position/normal/texcoord and the
// batched geometry as position/color, but both put position first at offset
// zero, and a vertex buffer's stride is a fact about the buffer rather than
// about this declaration. So the two layouts differ in everything this shader
// doesn't read, and it never has to know which one it was handed.

cbuffer Constants : register(b0)
{
    // World through the sun's view and projection, already transposed on the
    // CPU — DirectXMath writes rows and HLSL reads columns, and transposing
    // once per draw is cheaper than fighting about it per vertex.
    float4x4 LightWorldViewProj;
};

float4 main(float3 pos : POSITION) : SV_Position
{
    return mul(float4(pos, 1.0f), LightWorldViewProj);
}
