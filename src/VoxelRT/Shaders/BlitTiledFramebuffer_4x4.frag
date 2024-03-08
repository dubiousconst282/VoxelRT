in vec2 v_FragCoord;
out vec4 o_FragColor;

readonly buffer ssbo_FrameData {
    uint Width, Height, TileStride;
    uint Pixels[];
};

uint GetPixelOffset(uint x, uint y) {
    const uint TileSize = 4, TileShift = 2, TileMask = TileSize - 1, TileNumPixels = TileSize * TileSize;

    uint tileId = (x >> TileShift) + (y >> TileShift) * TileStride;
    uint pixelOffset = (x & TileMask) + (y & TileMask) * TileSize;
    return tileId * TileNumPixels + pixelOffset;
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
}