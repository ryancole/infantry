// One oversized triangle covering the viewport, built from the vertex id with
// no vertex buffer bound at all. Three vertices rather than a quad's six: a
// quad seams down its diagonal, where the two triangles' pixels are shaded by
// separate quads and anything reading neighbouring texels can disagree across
// the join. Draw with DrawInstanced(3, 1, 0, 0) on a triangle list.

void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    // (0,0), (2,0), (0,2) — the third of the triangle that lands on screen is
    // exactly the viewport, and uv comes out in the texture convention with the
    // origin at the top left.
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
