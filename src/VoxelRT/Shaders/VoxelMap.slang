public struct VoxelStorage {
    Material Palette[256];
    uint BaseSlots[VoxelMap.NUM_SECTORS];
    uint64_t BrickMasks[VoxelMap.NUM_SECTORS];
    uint64_t SectorMasks[VoxelMap.NUM_SECTORS / 64];
    uint8_t BrickVoxelData[];
};

public struct VoxelMap {
    public static const uint
        BRICK_SIZE = 8,
        NUM_SECTORS_XZ = 4096 / (BRICK_SIZE * 4),
        NUM_SECTORS_Y = 2048 / (BRICK_SIZE * 4);

    public static const uint
        GRID_SIZE_XZ = BRICK_SIZE * NUM_SECTORS_XZ * 4,
        GRID_SIZE_Y = BRICK_SIZE * NUM_SECTORS_Y * 4,

        BRICK_STRIDE = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE,
        OCC_STRIDE = (BRICK_SIZE / 4) * (BRICK_SIZE / 4) * (BRICK_SIZE / 4),

        NUM_SECTORS = NUM_SECTORS_XZ * NUM_SECTORS_XZ * NUM_SECTORS_Y;

    public VoxelStorage* Storage;
    public uint64_t* VoxelOccupancy;

    public static bool CheckBounds(int3 pos) {
        return uint(pos.x | pos.z) < GRID_SIZE_XZ && uint(pos.y) < GRID_SIZE_Y;
    }
    public uint GetBrickVoxelId(uint brickSlot, uint3 pos) {
        uint offset = GetLinearIndex(pos, BRICK_SIZE, BRICK_SIZE);
        return Storage->BrickVoxelData[brickSlot * BRICK_STRIDE + offset];
    }

    public Optional<uint> GetBrickDataSlot(uint3 brickPos) {
        uint sectorIdx = GetLinearIndex(brickPos / 4, NUM_SECTORS_XZ, NUM_SECTORS_Y);
        uint brickIdx = GetLinearIndex(brickPos, 4, 4);
        uint64_t allocMask = Storage->BrickMasks[sectorIdx];

        if ((allocMask >> brickIdx & 1) != 0) {
            return Storage->BaseSlots[sectorIdx] + PopCntVar64(allocMask, brickIdx);
        }
        return none;
    }

    public uint64_t GetVoxelMask(uint3 pos) {
        uint brickSlot = GetBrickDataSlot(pos / 8).value;
        uint cellOffset = GetLinearIndex(pos / 4, BRICK_SIZE / 4, BRICK_SIZE / 4);
        return VoxelOccupancy[brickSlot * OCC_STRIDE + cellOffset];
    }
    public uint64_t GetBrickMask(uint3 sectorPos) {
        return Storage->BrickMasks[GetLinearIndex(sectorPos, NUM_SECTORS_XZ, NUM_SECTORS_Y)];
    }
    public uint64_t GetSectorMask(uint3 sectorPos) {
        return Storage->SectorMasks[GetLinearIndex(sectorPos / 4, NUM_SECTORS_XZ / 4, NUM_SECTORS_Y / 4)];
    }

    public Material GetVoxelMaterial(uint3 pos) {
        Optional<uint> brickSlot = GetBrickDataSlot(pos / 8);
        if (!brickSlot.hasValue) {
            return Material();
        }
        return Storage->Palette[GetBrickVoxelId(brickSlot.value, pos)];
    }
};

// Count number of set bits in range [0..width]
public uint PopCntVar64(uint64_t mask, uint width) {
    uint himask = uint(mask);
    uint count = 0;

    if (width >= 32) {
        count = countbits(himask);
        himask = uint(mask >> 32);
    }
    uint m = 1u << (width & 31u);
    count += countbits(himask & (m - 1u));

    return count;
}

// Returns linear index for 3D grid in XZY order. Sizes **must** be power of two.
public uint GetLinearIndex(uint3 pos, uint sizeXZ, uint sizeY) {
    return (pos.x & (sizeXZ - 1)) +
           (pos.z & (sizeXZ - 1)) * sizeXZ +
           (pos.y & (sizeY - 1)) * (sizeXZ * sizeXZ);
}
public uint3 GetPosFromLinearIndex(uint idx, uint sizeXZ, uint sizeY) {
    uint3 pos;
    pos.x = idx % sizeXZ;
    pos.z = (idx / sizeXZ) % sizeXZ;
    pos.y = (idx / sizeXZ) / sizeXZ;
    return pos;
}

public struct Material {
    uint2 Data;

    public property float3 Color {
        get {
            uint3 mask = uint3(31, 63, 31);
            float3 color = float3(Data.xxx >> uint3(11, 5, 0) & mask) / float3(mask);
            return color * color;  // srgb gamma hack
        }
    }
    public property float Emission {
        get {
            return f16tof32(Data.x >> 16);
        }
    }
    public property float MetalFuzziness {
        get {
            return (Data.y & 255) / 255.0;
        }
    }
};
