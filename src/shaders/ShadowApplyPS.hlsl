// Reads the scene back and darkens what the sun couldn't reach.
//
// The alternative was teaching every effect in the renderer to sample a shadow
// map, and the toolkit's effects ship their pixel shaders precompiled, so that
// meant replacing all of them. Doing it here instead costs one fullscreen pass
// and leaves BasicEffect and NormalMapEffect exactly as they were: the depth
// buffer already knows where every opaque surface in the frame is, which is the
// only thing a shadow test actually needs from the geometry.
//
// It writes no color of its own. The pipeline blends with dest * src, so what
// this returns is the fraction of itself each pixel keeps.

cbuffer Constants : register(b0)
{
    float4x4 InvViewProj;   // clip -> world, for the pixel we're standing on
    float4x4 LightViewProj; // world -> the sun's clip space
    float4 Params;          // x: lit fraction in full shadow, y: depth bias,
                            // z: shadow texel size, w: unused
};

Texture2D<float> SceneDepth : register(t0);
Texture2D<float> ShadowMap : register(t1);
SamplerComparisonState ShadowSampler : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    // Loaded by pixel rather than sampled by uv: the depth buffer is exactly
    // viewport-sized and there is nothing to gain from filtering a value that
    // is about to be turned into a position.
    const float depth = SceneDepth.Load(int3(pos.xy, 0));

    // Cleared depth means nothing was drawn here — the sky past the arena's
    // edge. There is no surface to shade, so leave the pixel alone.
    if (depth >= 1.0f)
        discard;

    float4 world = mul(float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f), InvViewProj);
    world /= world.w;

    const float4 lightClip = mul(float4(world.xyz, 1.0f), LightViewProj);
    const float3 lightNdc = lightClip.xyz / lightClip.w;
    const float2 shadowUv = float2(lightNdc.x * 0.5f + 0.5f, 0.5f - lightNdc.y * 0.5f);

    // Off the edge of the map, or past the far plane the depth pass rendered
    // to. Both mean the same thing — nothing was recorded about this point —
    // and the honest answer to "was it in shadow" is no. The map is fitted to
    // the visible ground every frame, so in practice this is the handful of
    // pixels on tall geometry leaning out past the fit.
    if (any(shadowUv < 0.0f) || any(shadowUv > 1.0f) || lightNdc.z > 1.0f)
        return 1.0f;

    // Three by three, because one tap gives a shadow with the shadow map's own
    // staircase along its edge and the arena is watched from close enough to
    // read it. The comparison sampler does the depth test inside the filter, so
    // these are nine already-blended answers rather than nine yes-or-nos.
    const float compare = lightNdc.z - Params.y;
    float sum = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += ShadowMap.SampleCmpLevelZero(ShadowSampler,
                                                shadowUv + float2(x, y) * Params.z, compare);

    const float factor = lerp(Params.x, 1.0f, sum / 9.0f);
    return float4(factor, factor, factor, 1.0f);
}
