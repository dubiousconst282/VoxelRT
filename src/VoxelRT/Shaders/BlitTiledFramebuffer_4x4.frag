in vec2 v_FragCoord;
out vec4 o_FragColor;

readonly buffer ssbo_FrameData {
    uint Width, Height, TileStride;
    uint Pixels[];
};
layout(rgba16f) uniform image2D u_AccumTex;
uniform float u_BlendWeight;

uint GetPixelOffset(uint x, uint y) {
    const uint TileSize = 4, TileShift = 2, TileMask = TileSize - 1, TileNumPixels = TileSize * TileSize;

    uint tileId = (x >> TileShift) + (y >> TileShift) * TileStride;
    uint pixelOffset = (x & TileMask) + (y & TileMask) * TileSize;
    return tileId * TileNumPixels + pixelOffset;
}
vec3 aces_approx(vec3 v)
{
    v *= 0.6f;
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return clamp((v*(a*v+b))/(v*(c*v+d)+e), 0.0f, 1.0f);
}

void main() {
    uvec2 pos = uvec2(v_FragCoord * vec2(Width, Height));
    uint packedColor = Pixels[GetPixelOffset(pos.x, pos.y)];

#if FORMAT_RGB10
    uvec3 color = uvec3(packedColor) >> uvec3(22, 12, 2) & 1023u;
    o_FragColor.rgb = vec3(color) * (1.0 / 1023.0);
#else
    uvec3 color = uvec3(packedColor) >> uvec3(0, 8, 16) & 255u;
    o_FragColor.rgb = vec3(color) * (1.0 / 255.0);
#endif
    o_FragColor.a = 1.0;

    o_FragColor *= 1 / 0.25;
    
    vec3 prevColor = imageLoad(u_AccumTex, ivec2(pos)).rgb;
    o_FragColor.rgb = mix(prevColor.rgb, o_FragColor.rgb, u_BlendWeight);
    imageStore(u_AccumTex, ivec2(pos), o_FragColor);
    
    o_FragColor.rgb=aces_approx(pow(o_FragColor.rgb, vec3(0.8)) * 1.9);
}