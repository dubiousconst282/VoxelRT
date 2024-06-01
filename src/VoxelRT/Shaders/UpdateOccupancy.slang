import VoxelMap;

struct DispatchParams {
    uint NumBricks;
    uint3* BrickLocs;
    VoxelMap Map;
};
[vk::push_constant] ConstantBuffer<DispatchParams> pc;

[numthreads(64)]
void ComputeMain(uint idx: SV_DispatchThreadID) {
    if (idx >= pc.NumBricks) return;

    uint3 brickPos = pc.BrickLocs[idx];
    uint baseSlot = pc.Map.GetBrickDataSlot(brickPos).value;

    for (uint cy = 0; cy < VoxelMap.BRICK_SIZE; cy += 4)
    for (uint cz = 0; cz < VoxelMap.BRICK_SIZE; cz += 4)
    for (uint cx = 0; cx < VoxelMap.BRICK_SIZE; cx += 4) {
        uint mask1 = 0, mask2 = 0;

        for (uint vy = 0; vy < 2; vy++)
        for (uint vz = 0; vz < 4; vz++)
        for (uint vx = 0; vx < 4; vx++) {
            bool occupied1 = pc.Map.GetBrickVoxelId(baseSlot, uint3(cx + vx, cy + vy + 0, cz + vz)) != 0;
            mask1 |= uint(occupied1) << (vx + vz * 4 + vy * 16);

            bool occupied2 = pc.Map.GetBrickVoxelId(baseSlot, uint3(cx + vx, cy + vy + 2, cz + vz)) != 0;
            mask2 |= uint(occupied2) << (vx + vz * 4 + vy * 16);
        }
        uint cellOffset = GetLinearIndex(uint3(cx, cy, cz) / 4u, VoxelMap.BRICK_SIZE / 4, VoxelMap.BRICK_SIZE / 4);
        pc.Map.VoxelOccupancy[baseSlot * VoxelMap.OCC_STRIDE + cellOffset] = mask1 | uint64_t(mask2) << 32;
    }
}