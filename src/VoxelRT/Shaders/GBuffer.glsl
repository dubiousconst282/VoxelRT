layout(rgba8) uniform image2D u_AlbedoNormalTex; // rgb: albedo, w: packed normal
layout(rgba16f) uniform image2D u_IrradianceTex; // rgb: irradiance, w: variance
layout(r32f) uniform image2D u_DepthTex;
layout(rg16f) uniform image2D u_MomentsTex;

layout(r8ui) uniform uimage2D u_HistoryLenTex;

uniform sampler2D u_PrevAlbedoNormalTex;
uniform sampler2D u_PrevIrradianceTex;
uniform sampler2D u_PrevDepthTex;
uniform sampler2D u_PrevMomentsTex;

#define g_RenderSize imageSize(u_AlbedoNormalTex)

vec3 unpackGNormal(float value) {
    return vec3(uvec3(value * 255 + 0.5) >> uvec3(0, 2, 4) & 3u) - 1;
}
float packGNormal(vec3 normal) {
    uvec3 inorm = uvec3(clamp(normal + 1, 0, 3));
    return float(inorm.x << 0 | inorm.y << 2 | inorm.z << 4) / 255.0;
}

bool gbufferCheckBounds(ivec2 pos) {
    return all(lessThan(uvec2(pos), uvec2(g_RenderSize)));
}