import VoxelMap;

struct SectorUpdateRecord {
    uint32_t SectorIdx;
    SectorInfo AllocInfo;
    uint64_t AllocMask;
    uint64_t DirtyMask;
    uint8_t* SourceData;
};
struct DispatchParams {
    uint NumRecords;
    SectorUpdateRecord* Records;
    VoxelMap Map;
};
[vk::push_constant] DispatchParams pc;

void GenerateVoxelMasks(uint8_t* source, uint64_t* dest) {
    for (uint cy = 0; cy < VoxelMap.BRICK_SIZE; cy += 4)
    for (uint cz = 0; cz < VoxelMap.BRICK_SIZE; cz += 4)
    for (uint cx = 0; cx < VoxelMap.BRICK_SIZE; cx += 4) {
        uint mask1 = 0, mask2 = 0;

        for (uint vy = 0; vy < 2; vy++)
        for (uint vz = 0; vz < 4; vz++)
        for (uint vx = 0; vx < 4; vx++) {
            bool occupied1 = source[GetLinearIndex(uint3(cx + vx, cy + vy + 0, cz + vz), VoxelMap.BRICK_SIZE, VoxelMap.BRICK_SIZE)] != 0;
            mask1 |= uint(occupied1) << (vx + vz * 4 + vy * 16);

            bool occupied2 = source[GetLinearIndex(uint3(cx + vx, cy + vy + 2, cz + vz), VoxelMap.BRICK_SIZE, VoxelMap.BRICK_SIZE)] != 0;
            mask2 |= uint(occupied2) << (vx + vz * 4 + vy * 16);
        }
        uint cellOffset = GetLinearIndex(uint3(cx, cy, cz) / 4u, VoxelMap.BRICK_SIZE / 4, VoxelMap.BRICK_SIZE / 4);
        dest[cellOffset] = mask1 | uint64_t(mask2) << 32;
    }
}

[numthreads(64)]
void ComputeMain(uint idx: SV_DispatchThreadID) {
    if (idx >= pc.NumRecords) return;

    SectorUpdateRecord rec = pc.Records[idx];

    // Update headers
    VoxelStorage* storage = pc.Map.Storage;
    storage->AllocMasks[rec.SectorIdx] = rec.AllocMask;
    storage->Sectors[rec.SectorIdx] = rec.AllocInfo;

    uint3 sectorPos = GetPosFromLinearIndex(rec.SectorIdx, VoxelMap.NUM_SECTORS_XZ, VoxelMap.NUM_SECTORS_Y);
    uint64_t* sectorMask = &storage->SectorMasks[GetLinearIndex(sectorPos / 4, VoxelMap.NUM_SECTORS_XZ / 4, VoxelMap.NUM_SECTORS_Y / 4)];
    uint64_t flag = 1ull << GetLinearIndex(sectorPos,4,4);
    
    if (rec.AllocMask != 0) {
        __spirvInterlocked_or(*sectorMask, flag);
        //InterlockedOr(*sectorMask, flag);
    } else {
        __spirvInterlocked_and(*sectorMask, ~flag);
        //InterlockedAnd(*sectorMask, ~flag);
    }

    // Copy incoming data
    uint8_t* storageData = &pc.Map.Storage->BrickVoxelData[rec.AllocInfo.BaseAddress];
    uint numPages = (popcnt64(rec.AllocMask) * rec.AllocInfo.BrickStride) / 16u;

    for (uint i = 0; i < numPages; i++) {
        ((uint64_t2*)storageData)[i] = ((uint64_t2*)rec.SourceData)[i];
    }

    // Generate accel masks
    uint allocIdx = 0;
    for (uint64_t mask = rec.AllocMask; mask != 0; mask &= mask - 1) {
        uint brickIdx = tzcnt64(mask);
        GenerateVoxelMasks(
            &storageData[allocIdx * rec.AllocInfo.BrickStride],
            &pc.Map.VoxelOccupancy[rec.AllocInfo.BaseAddress / 64 + allocIdx * VoxelMap.OCC_STRIDE]);
        allocIdx++;
    }
}