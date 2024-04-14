layout(rgba32ui) uniform uimage2D u_GBuffer;
layout(rgba32ui) uniform uimage2D u_HistoryBuffer; // may be unbound

uniform vec3 u_OriginFrac;

struct GSample {
    vec3 albedo;        // u8 x 3
    vec3 normal;        // u8
    float depth;        // f32
    vec3 irradiance;    // f16 x 3
};

GSample gbufferLoad(ivec2 pos, bool history) {
    uvec4 data = history ? imageLoad(u_HistoryBuffer, pos) : imageLoad(u_GBuffer, pos);

    GSample g;
    g.albedo = vec3(data.xxx >> uvec3(0, 8, 16) & 255u) * (1.0 / 255);
    g.normal = vec3(data.xxx >> uvec3(24, 26, 28) & 3u) - 1;
    g.depth = uintBitsToFloat(data.y);

    vec2 half_w = unpackHalf2x16(data.w);
    g.irradiance = vec3(unpackHalf2x16(data.z), half_w.x);

    return g;
}
void gbufferStore(ivec2 pos, GSample g) {
    uvec3 albedo = uvec3(clamp(g.albedo * 255, 0, 255));
    uvec3 norm = uvec3(clamp(g.normal + 1, 0, 3));

    uvec4 data;
    data.x = albedo.x << 0 | albedo.y << 8 | albedo.z << 16 |
             norm.x << 24 | norm.y << 26 | norm.z << 28;
    data.y = floatBitsToUint(g.depth);
    data.z = packHalf2x16(g.irradiance.xy);
    data.w = packHalf2x16(vec2(g.irradiance.z));

    imageStore(u_GBuffer, pos, data);
}

void getPrimaryRay(vec2 uv, mat4 invProjMat, out vec3 rayPos, out vec3 rayDir) {
    vec4 near = invProjMat * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vec4 far = near + invProjMat[2];
    rayPos = vec3(near * (1.0 / near.w)) + u_OriginFrac;
    rayDir = normalize(vec3(far * (1.0 / far.w)));
}