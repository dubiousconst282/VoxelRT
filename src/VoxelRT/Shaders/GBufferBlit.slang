#include <Havk/FullscreenPass.slang>

import Havk.Core;
import GBuffer;

enum DebugChannel { None, Albedo, Irradiance, Normals, HeatMap, Variance };

struct DispatchParams {
    GBufferUniforms* GBuffer;
    DebugChannel Channel;
    float2 HeatmapRange;
};
[vk::push_constant] DispatchParams pc;

[vk::binding(0, 1)]
[SamplerDesc(MagFilter.Linear, MinFilter.Linear, MipFilter.Linear, WrapMode.Repeat)]
SamplerState g_LinearSampler;

// TODO: Port to compute
[shader("fragment")]
float4 FragmentMain(float2 uv: TEXCOORD0) {
    uv.y = 1.0-uv.y;
    int2 pos = int2(uv * pc.GBuffer->RenderSize);
    // float3 albedo = pc.GBuffer->AlbedoTex.Instance[pos].rgb;
    // float3 irradiance = pc.GBuffer.IrradianceTex.Instance[pos].rgb;

    // This is fine as long image was created with USE_SAMPLED
    float3 albedo = reinterpret<TextureHandle2D>(pc.GBuffer->AlbedoTex).Instance.SampleLevel(g_LinearSampler, uv, 0).rgb;
    float3 irradiance = reinterpret<TextureHandle2D>(pc.GBuffer->IrradianceTex).Instance.SampleLevel(g_LinearSampler, uv, 0).rgb;

    switch (pc.Channel) {
        default: {            
            float3 color = albedo * irradiance;

            color *= 0.48;
            color = aces_approx(color);
            color = pow(color, 0.45);

            return float4(color, 1.0);
        }
        case DebugChannel.Albedo: {
            return float4(albedo, 1.0);
        }
        case DebugChannel.Irradiance: {
            return float4(aces_approx(irradiance), 1.0);
        }
        case DebugChannel.Normals: {
            float3 normal = UnpackAlbedo(pc.GBuffer.AlbedoTex.Instance[pos]).Normal;
            return float4(normal * 0.5 + 0.5, 1.0);
        }
        case DebugChannel.HeatMap: {
            // radiance.w == renderClocks unless overwritten by reproj / SVGF passes.
            float iters = pc.GBuffer.IrradianceTex.Instance[pos].w;
            return float4(viridis_inferno(iters, pc.HeatmapRange), 1);
        }
        case DebugChannel.Variance: {
            float variance = pc.GBuffer.IrradianceTex.Instance[pos].w;
            variance = sqrt(variance) * 3;
            // variance = imageLoad(u_DepthTex, pos).r * 0.1;
            return float4(variance.xxx, 1.0);
        }
    }
}


float3 aces_approx(float3 v) {
    v *= 0.6f;
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return clamp((v*(a*v+b))/(v*(c*v+d)+e), 0.0f, 1.0f);
}

// Viridis approximation, Jerome Liard, August 2016
// https://www.shadertoy.com/view/XtGGzG
float3 viridis_inferno( float x, float2 range )
{
	x = saturate((x - range.x) / (range.y - range.x));
	float4 x1 = float4( 1.0, x, x * x, x * x * x ); // 1 x x2 x3
	float4 x2 = x1 * x1.w * x; // x4 x5 x6 x7
	return float3(
		dot( x1.xyzw, float4( -0.027780558, +1.228188385, +0.278906882, +3.892783760 ) ) + dot( x2.xy, float2( -8.490712758, +4.069046086 ) ),
		dot( x1.xyzw, float4( +0.014065206, +0.015360518, +1.605395918, -4.821108251 ) ) + dot( x2.xy, float2( +8.389314011, -4.193858954 ) ),
		dot( x1.xyzw, float4( -0.019628385, +3.122510347, -5.893222355, +2.798380308 ) ) + dot( x2.xy, float2( -3.608884658, +4.324996022 ) ) );
}