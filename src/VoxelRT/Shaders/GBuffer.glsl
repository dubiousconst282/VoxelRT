layout(rgba32ui) uniform uimage2D u_BackBuffer;
layout(rgba32ui) uniform uimage2D u_FrontBuffer; // may be unbound

uniform vec3 u_OriginFrac;

struct GSample {
    vec3 albedo;        // u8 x 3
    vec3 normal;        // u8
    float depth;        // f32
    vec3 irradiance;    // f16 x 3
    float variance;     // f16
    float historyWeight; // frac u4
};

GSample gbufferLoad(ivec2 pos, bool back) {
    uvec4 data = back ? imageLoad(u_BackBuffer, pos) : imageLoad(u_FrontBuffer, pos);

    GSample g;
    g.albedo = vec3(data.xxx >> uvec3(0, 8, 16) & 255u) * (1.0 / 255);
    g.normal = vec3(data.xxx >> uvec3(24, 26, 28) & 3u) - 1;
    g.depth = uintBitsToFloat(data.y & ~15u);

    vec2 half_w = unpackHalf2x16(data.w);
    g.irradiance = vec3(unpackHalf2x16(data.z), half_w.x);
    g.variance = half_w.y;
    g.historyWeight = float(data.y&15u)*(1.0/15);

    return g;
}
// TODO: this is garbage, replace with separate textures and catmul-rom: 
// - https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/
// - https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
GSample gbufferSample(vec2 uv) {
    uv *= imageSize(u_FrontBuffer);
    uv -= 0.5;
    ivec2 pos = ivec2(floor(uv));
    vec2 f = fract(uv);

    GSample s00 = gbufferLoad(pos + ivec2(0,0),false);
    GSample s10 = gbufferLoad(pos + ivec2(1,0),false);
    GSample s01 = gbufferLoad(pos + ivec2(0,1),false);
    GSample s11 = gbufferLoad(pos + ivec2(1,1),false);

    GSample s;
    s.irradiance = mix(mix(s00.irradiance, s10.irradiance, f.x),
                       mix(s01.irradiance, s11.irradiance, f.x), f.y);
    s.albedo = mix(mix(s00.albedo, s10.albedo, f.x),
                  mix(s01.albedo, s11.albedo, f.x), f.y);
    s.normal = mix(mix(s00.normal, s10.normal, f.x),
                       mix(s01.normal, s11.normal, f.x), f.y);

    return s;
}
void gbufferStore(ivec2 pos, GSample g) {
    uvec3 albedo = uvec3(clamp(g.albedo * 255, 0, 255));
    uvec3 norm = uvec3(clamp(g.normal + 1, 0, 3));

    uvec4 data;
    data.x = albedo.x << 0 | albedo.y << 8 | albedo.z << 16 |
             norm.x << 24 | norm.y << 26 | norm.z << 28;
    data.y = floatBitsToUint(g.depth) & ~15u | uint(clamp(g.historyWeight, 1.0 / 15, 1.0) * 15);
    data.z = packHalf2x16(g.irradiance.xy);
    data.w = packHalf2x16(vec2(g.irradiance.z, g.variance));

    imageStore(u_BackBuffer, pos, data);
}


void getPrimaryRay(vec2 uv, mat4 invProjMat, out vec3 rayPos, out vec3 rayDir) {
    vec4 near = invProjMat * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vec4 far = near + invProjMat[2];
    rayPos = vec3(near * (1.0 / near.w)) + u_OriginFrac;
    rayDir = normalize(vec3(far * (1.0 / far.w)));
}