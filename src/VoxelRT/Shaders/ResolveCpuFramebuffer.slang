import GBuffer;

struct Framebuffer {
    uint Width, Height, Stride;
    uint TileShiftX, TileShiftY;
    uint Padding[11];
    uint PixelData[];

    [__ref]
    uint GetPixelOffset(uint2 pos) {
        uint FieldStride = 1u << (TileShiftX + TileShiftY);
        uint TileStride = (FieldStride * 4 + 15) & ~15;
        uint TileMaskX = (1u << TileShiftX) - 1;
        uint TileMaskY = (1u << TileShiftY) - 1;

        uint tileId = (pos.x >> TileShiftX) + (pos.y >> TileShiftY) * Stride;
        uint pixelOffset = (pos.x & TileMaskX) + ((pos.y & TileMaskY) << TileShiftX);
        return tileId * TileStride + pixelOffset;
    }
};

struct DispatchParams {
    GBufferUniforms* GBuffer;
    Framebuffer* Input;
};
[vk::push_constant] ConstantBuffer<DispatchParams> pc;

[numthreads(8, 8, 1)]
void ComputeMain(uint2 pos: SV_DispatchThreadID) {
    uint FieldStride = 1u << (pc.Input->TileShiftX + pc.Input->TileShiftY);
    uint offset = pc.Input->GetPixelOffset(pos);
    uint* data = &pc.Input->PixelData[offset];

    float3 albedo = float3(data[FieldStride * 0].xxx >> uint3(0, 8, 16) & 255) / 255.0;
    float3 normal = float3(data[FieldStride * 0].xxx >> uint3(24, 26, 28) & 3) - 1;
    float depth = bit_cast<float>(data[FieldStride * 1]);
    float3 irradiance = f16tof32(uint3(data[FieldStride * 2].xx, data[FieldStride * 3].x) >> uint3(0, 16, 0) & 65535);

    if (depth < 0) albedo = 1.0;

    pc.GBuffer->AlbedoTex.Instance[pos] = float4(albedo, PackGNormal(normal));
    pc.GBuffer->IrradianceTex.Instance[pos] = float4(irradiance, 0);
}