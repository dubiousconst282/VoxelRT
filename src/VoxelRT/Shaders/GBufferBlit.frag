in vec2 v_FragCoord;
out vec4 o_FragColor;

uniform int u_DebugChannel;

#include "GBuffer.glsl"

vec3 aces_approx(vec3 v) {
    v *= 0.6f;
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return clamp((v*(a*v+b))/(v*(c*v+d)+e), 0.0f, 1.0f);
}

void main() {
    ivec2 pos = ivec2(v_FragCoord * g_RenderSize);
    vec3 albedo = imageLoad(u_AlbedoNormalTex, pos).rgb;
    vec3 irradiance = imageLoad(u_IrradianceTex, pos).rgb;
    //irradiance = texelFetch(u_PrevIrradianceTex, pos, 0).rgb;

    vec3 color = albedo * irradiance;

    color = aces_approx(pow(color, vec3(0.7)));

    if (u_DebugChannel == 1) {
        color = albedo;
    } else if (u_DebugChannel == 2) {
        color = aces_approx(irradiance);
    } else if (u_DebugChannel == 3) {
        color = unpackGNormal(imageLoad(u_AlbedoNormalTex, pos).w) * 0.5 + 0.5;
    } else if (u_DebugChannel == 4) {
        // radiance.w == rayStepIters unless overwritten by reproj / SVGF passes.
        float iters = imageLoad(u_IrradianceTex, pos).w;
        color = iters < 64 ? vec3(iters / 64.0) : mix(vec3(1.0), vec3(1.0, 0, 0), (iters - 64) / 128.0);
    } else if (u_DebugChannel == 5) {
        float variance = imageLoad(u_IrradianceTex, pos).w;
        variance = sqrt(variance) * 3;
        // variance = imageLoad(u_DepthTex, pos).r * 0.1;
        color = vec3(variance);
    }
    o_FragColor = vec4(color, 1.0);
}