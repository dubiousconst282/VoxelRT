in vec2 v_FragCoord;
out vec4 o_FragColor;

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
    ivec2 pos = ivec2(v_FragCoord * imageSize(u_GBuffer));
    GSample g = gbufferLoad(pos, false);

    vec3 color = g.albedo * g.irradiance;

    color = aces_approx(pow(color, vec3(0.7)));

    o_FragColor = vec4(color, 1.0);
}