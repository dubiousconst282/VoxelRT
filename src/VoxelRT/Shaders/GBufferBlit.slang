#include <Havk/FullscreenPass.slang>

import Havk.Core;
import GBuffer;

enum DebugChannel { None, Albedo, Irradiance, Normals, TraversalIters, Variance };

struct DispatchParams {
    GBufferUniforms* GBuffer;
    DebugChannel Channel;
    uint padding;
};
[vk::push_constant] DispatchParams pc;

float3 aces_approx(float3 v) {
    v *= 0.6f;
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return clamp((v*(a*v+b))/(v*(c*v+d)+e), 0.0f, 1.0f);
}

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
            float3 normal = UnpackGNormal(pc.GBuffer.AlbedoTex.Instance[pos].w);
            return float4(normal * 0.5 + 0.5, 1.0);
        }
        case DebugChannel.TraversalIters: {
            // radiance.w == rayStepIters unless overwritten by reproj / SVGF passes.
            float iters = pc.GBuffer.IrradianceTex.Instance[pos].w;

            if (iters < 64) return float4(iters.xxx / 64.0, 1.0);
            return lerp(float4(1), float4(1, 0, 0, 1), (iters - 64) / 128.0);
        }
        case DebugChannel.Variance: {
            float variance = pc.GBuffer.IrradianceTex.Instance[pos].w;
            variance = sqrt(variance) * 3;
            // variance = imageLoad(u_DepthTex, pos).r * 0.1;
            return float4(variance.xxx, 1.0);
        }
    }
}